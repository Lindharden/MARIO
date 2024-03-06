#include <stdlib.h>
#include <dlfcn.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <param/param.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include <sys/shm.h>
#include "pipeline.h"
#include "module_config.pb-c.h"
#include "pipeline_config.pb-c.h"
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

size_t get_param_buffer(uint8_t **out, param_t *param)
{
    // initialize buffer for module parameters
    int initial_buf_size = DATA_PARAM_SIZE;
    uint8_t buf[initial_buf_size];
    param_get_data(param, buf, initial_buf_size);
    int buf_size = (int)buf[0];

    *out = malloc(buf_size * sizeof(uint8_t));
    if (!*out) {
        return 0;
    }

    // Copy the data from the original buffer to the new buffer
    for (size_t i = 0; i < buf_size; i++)
    {
        (*out)[i] = buf[i + 1];
    }

    return buf_size;
}

// Function to load a module and parameter from a configuration file
void *load_module(char *moduleName)
{
    char filename[256]; // Adjust the buffer size as needed
    snprintf(filename, sizeof(filename), "/usr/share/pipeline/%s.so", moduleName);

    // Load the external library dynamically
    void *handle = dlopen(filename, RTLD_LAZY);
    if (handle == NULL)
    {
        fprintf(stderr, "Error: Unable to load the library %s.\n", filename);
        return NULL;
    }

    // Get a function pointer to the external function
    void *functionPointer = dlsym(handle, "run");
    if (functionPointer == NULL)
    {
        fprintf(stderr, "Error: Unable to find the run function in %s.\n", filename);
        dlclose(handle);
        return NULL;
    }

    return functionPointer;
}

void setup() {
    uint8_t *buffer = NULL; // Initialize buffer pointer to NULL
    size_t buf_size = 0;

    for (size_t pipeline_idx = 0; pipeline_idx < MAX_PIPELINES; pipeline_idx++) {
        buf_size = get_param_buffer(&buffer, pipeline_configs[pipeline_idx]);

        PipelineDefinition *pdef = pipeline_definition__unpack(NULL, buf_size, buffer);
        if (!pdef) {
            free(buffer);
            continue; // Skip this pipeline if unpacking fails
        }

        pipelines[pipeline_idx].pipeline_id = pdef->id;
        pipelines[pipeline_idx].num_modules = pdef->n_modules;

        for (size_t module_idx = 0; module_idx < pdef->n_modules; module_idx++) {
            // Reset buffer for the next use
            free(buffer);
            buffer = NULL;

            ModuleDefinition *mdef = pdef->modules[module_idx];

            // Cache the parsed pipeline values
            pipelines[pipeline_idx].modules[module_idx].module_name = strdup(mdef->name);
            pipelines[pipeline_idx].modules[module_idx].module_function = load_module(mdef->name);
            size_t moduleBufSize = get_param_buffer(&buffer, module_configs[mdef->param_id - 1]);
            pipelines[pipeline_idx].modules[module_idx].module_param = module_config__unpack(NULL, moduleBufSize, buffer);
        }
        
        free(buffer);
        buffer = NULL;
    }
}

void callback_run(param_t *param, int index)
{
    if (param_get_uint8(param) > 0)
    {
        run_pipeline();
        param_set_uint8(param, 0);
    }
}

int execute_module_in_process(ProcessFunction func, ImageBatch *input, int *outputPipe, ModuleConfig *config)
{
    // Create a new process
    pid_t pid = fork();

    if (pid == 0)
    {
        // Child process: Execute the module function
        ImageBatch result = func(input, config);
        size_t data_size = sizeof(result);
        write(outputPipe[1], &result, data_size); // Write the result to the pipe
        exit(EXIT_SUCCESS);
    }
    else
    {
        // Parent process: Wait for the child process to finish
        int status;
        waitpid(pid, &status, 0);

        if (WIFEXITED(status))
        {
            // Child process exited normally
            if (WEXITSTATUS(status) != 0)
            {
                fprintf(stderr, "Child process exited with non-zero status\n");
                exit(EXIT_FAILURE);
            }
        }
        else
        {
            // Child process did not exit normally
            fprintf(stderr, "Child process did not exit normally\n");
            return FAILURE;
        }

        return SUCCESS;
    }
}

int execute_pipeline(Pipeline *pipeline, ImageBatch *data)
{
    int outputPipe[2]; // Pipe for inter-process communication
    pipe(outputPipe);

    for (size_t i = 0; i < pipeline->num_modules; ++i)
    {
        ProcessFunction module_function = pipeline->modules[i].module_function;
        ModuleConfig *module_config = pipeline->modules[i].module_param;

        int module_status = execute_module_in_process(module_function, data, outputPipe, module_config);

        if (module_status == FAILURE)
        {
            close(outputPipe[0]); // Close the read end of the pipe
            close(outputPipe[1]); // Close the write end of the pipe
            return i + 1;
        }

        ImageBatch result;
        read(outputPipe[0], &result, sizeof(result)); // Read the result from the pipe
        data->channels = result.channels;
        data->width = result.width;
        data->height = result.height;
        data->shm_key = result.shm_key;
        data->num_images = result.num_images;
        data->pipeline_id = result.pipeline_id;
        data->data = result.data;
    }

    close(outputPipe[0]); // Close the read end of the pipe
    close(outputPipe[1]); // Close the write end of the pipe

    return SUCCESS;
}

void save_images(const char *filename_base, const ImageBatch *batch)
{
    int image_size = batch->width * batch->height * batch->channels;
    for (size_t i = 0; i < batch->num_images; i++)
    {
        char filename[20];
        sprintf(filename, "/usr/share/pipeline/%s%d.png", filename_base, i);

        // Determine the desired output format (e.g., PNG)
        int stride = batch->width * batch->channels;
        int success = stbi_write_png(filename, batch->width, batch->height, batch->channels, &batch->data[i * image_size], stride);
        if (!success)
        {
            fprintf(stderr, "Error writing image to %s\n", filename);
        }
        else
        {
            printf("Image saved as %s\n", filename);
        }
    }
}

int get_pipeline_by_id(int pipeline_id, Pipeline **pipeline)
{
    for (size_t i = 0; i < MAX_PIPELINES; i++)
    {
        if (pipelines[i].pipeline_id == pipeline_id)
        {
            *pipeline = &pipelines[i];
            return SUCCESS;
        }
    }
    return FAILURE;
}

void cleanup()
{
    // TODO: cleanup
}

void camera_sim()
{
    // Prepare the data
    int i = 300;
    ImageBatch data;
    data.mtype = 1;
    const char *filename = "/usr/share/pipeline/test.png"; 
    int image_width, image_height, image_channels;
    unsigned char *image_data = stbi_load(filename, &image_width, &image_height, &image_channels, STBI_rgb_alpha);
    data.height = image_height;
    data.width = image_width;
    data.channels = image_channels;
    data.num_images = 2;
    data.shm_key = i++; // testing key
    data.pipeline_id = 1;
    size_t image_size = image_height * image_width * image_channels;
    size_t data_size = image_size * data.num_images;
    int shmid = shmget(data.shm_key, data_size, IPC_CREAT | 0666);
    char *shmaddr = shmat(shmid, NULL, 0);
    memcpy(shmaddr, image_data, image_size); // Copy image batch data to shared memory
    memcpy(shmaddr + image_size, image_data, image_size); // Copy image batch data to shared memory

    // create msg queue
    int msg_queue_id;
    if ((msg_queue_id = msgget(75, 0666 | IPC_CREAT)) == -1)
    {
        perror("msgget error");
    }

    // send msg to queue
    if (msgsnd(msg_queue_id, &data, sizeof(data) - sizeof(long), 0) == -1)
    {
        perror("msgsnd error");
    }

    printf("Image sent!\n");
}

void run_pipeline(void)
{
    setup();
    // TODO: Check if pipelines and modules are cached, otherwise run setup()

    camera_sim();

    // Create msg queue
    int msg_queue_id;
    int MSG_QUEUE_KEY = 75;
    if ((msg_queue_id = msgget(MSG_QUEUE_KEY, 0)) == -1)
    {
        perror("Could not get MSG queue");
    }

    // Check if there are messages in the queue
    struct msqid_ds buf;
    if (msgctl(msg_queue_id, IPC_STAT, &buf) == -1)
    {
        perror("msgctl error");
    }

    if (buf.msg_qnum <= 0)
    {
        perror("No items in the msg queue");
        return;
    }

    // Recieve msg from queue
    ImageBatch datarcv;
    if (msgrcv(msg_queue_id, &datarcv, sizeof(ImageBatch) - sizeof(long), 1, 0) == -1)
    {
        perror("msgrcv error");
    }

    // Recieve shared memory id from recieved data
    int shmid;
    if ((shmid = shmget(datarcv.shm_key, 0, 0)) == -1)
    {
        perror("Could not get shared memory");
    }

    // Attach to shared memory from id
    int *shmaddr = shmat(shmid, NULL, 0);
    datarcv.data = shmaddr; // retrieve correct address in shared memory

    // Execute the pipeline with parameter values
    Pipeline *pipeline;
    if (get_pipeline_by_id(datarcv.pipeline_id, &pipeline) == FAILURE)
    {
        fprintf(stderr, "Pipeline with id '%s' does not exist.\n", datarcv.pipeline_id);
    }

    int status = execute_pipeline(pipeline, &datarcv);

    if (status != SUCCESS)
    {
        // Print failure message
        printf("Module named '%s' caused a failure in the pipeline\n", pipeline->modules[status - 1]);
        return;
    }

    save_images("output", &datarcv);

    // Detach and free shared memory
    shmdt(shmaddr);
    shmctl(shmid, IPC_RMID, NULL);
}

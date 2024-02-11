#include <stdlib.h>
#include <dlfcn.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <param/param.h>
#include <csp/csp.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include <sys/shm.h>
#include "types.h"
#include "pipeline.h"
#include "../param_config.h"
#include "../vmem_config.h"
#include "../protos/config.pb-c.h"
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

// Error codes
#define SUCCESS 0
#define FAILURE -1

/* Define module specific parameters */
static uint8_t _module_param_1 = 1;
static uint8_t _module_param_2 = 1;
static uint8_t _module_param_3 = 1;
static uint8_t _module_param_4 = 1;
static uint8_t _module_param_5 = 1;
static uint8_t _module_param_6 = 1;
PARAM_DEFINE_STATIC_RAM(PARAMID_MODULE_PARAM_1, module_param_1, PARAM_TYPE_UINT8, -1, 0, PM_CONF, NULL, NULL, &_module_param_1, "Module parameter");
PARAM_DEFINE_STATIC_RAM(PARAMID_MODULE_PARAM_2, module_param_2, PARAM_TYPE_UINT8, -1, 0, PM_CONF, NULL, NULL, &_module_param_2, "Module parameter");
PARAM_DEFINE_STATIC_RAM(PARAMID_MODULE_PARAM_3, module_param_3, PARAM_TYPE_UINT8, -1, 0, PM_CONF, NULL, NULL, &_module_param_3, "Module parameter");
PARAM_DEFINE_STATIC_RAM(PARAMID_MODULE_PARAM_4, module_param_4, PARAM_TYPE_UINT8, -1, 0, PM_CONF, NULL, NULL, &_module_param_4, "Module parameter");
PARAM_DEFINE_STATIC_RAM(PARAMID_MODULE_PARAM_5, module_param_5, PARAM_TYPE_UINT8, -1, 0, PM_CONF, NULL, NULL, &_module_param_5, "Module parameter");
PARAM_DEFINE_STATIC_RAM(PARAMID_MODULE_PARAM_6, module_param_6, PARAM_TYPE_UINT8, -1, 0, PM_CONF, NULL, NULL, &_module_param_6, "Module parameter");

void callback_run(param_t *param, int index)
{
    if (param_get_uint8(param) > 0)
    {
        run_pipeline();
        param_set_uint8(param, 0);
    }
}

PARAM_DEFINE_STATIC_VMEM(9, config_str, PARAM_TYPE_STRING, 200, 0, PM_CONF, NULL, NULL, config, VMEM_CONF_CONFIG, NULL);
PARAM_DEFINE_STATIC_VMEM(10, proto_data, PARAM_TYPE_DATA, 200, 0, PM_CONF, NULL, NULL, proto, VMEM_CONF_PROTO, NULL);

static param_t *params[] = {&module_param_1, &module_param_2, &module_param_3, &module_param_4, &module_param_5, &module_param_6};

/* Define a pipeline_run parameter */
static uint8_t _pipeline_run = 0;
PARAM_DEFINE_STATIC_RAM(PARAMID_PIPELINE_RUN, pipeline_run, PARAM_TYPE_UINT8, -1, 0, PM_CONF, callback_run, NULL, &_pipeline_run, "Set the pipeline to execute the file");

void initializePipeline(Pipeline *pipeline, ProcessFunction *funcs, size_t size)
{
    pipeline->functions = malloc(size * sizeof(ProcessFunction));
    memcpy(pipeline->functions, funcs, size * sizeof(ProcessFunction));
    pipeline->size = size;
}

int executeModuleInProcess(ProcessFunction func, ImageBatch *input, int *outputPipe, uint8_t paramValue)
{
    // Create a new process
    pid_t pid = fork();

    if (pid == 0)
    {
        // Child process: Execute the module function
        ImageBatch result = func(input, paramValue);
        size_t data_size = sizeof(result);
        write(outputPipe[1], &data_size, sizeof(data_size)); // Write size of result object
        write(outputPipe[1], &result, data_size);            // Write the result to the pipe
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

int get_buf_size(uint8_t *buf, size_t buf_size)
{
    size_t actual_size = 0;

    while (actual_size < buf_size && buf[actual_size] != 0)
    {
        actual_size++;
    }

    return actual_size;
}

void trim_buffer(uint8_t *buf, uint8_t *old_buf, size_t buf_size)
{
    // Copy the data from the original buffer to the new buffer
    for (size_t i = 0; i < buf_size; i++)
    {
        buf[i] = old_buf[i];
    }
}

int executePipeline(Pipeline *pipeline, ImageBatch *data, int values[])
{
    int outputPipe[2]; // Pipe for inter-process communication
    pipe(outputPipe);

    for (size_t i = 0; i < pipeline->size; ++i)
    {
        ProcessFunction func = pipeline->functions[i];

        // initialize buffer for module parameters
        int initial_buf_size = 200;
        uint8_t buf[initial_buf_size];
        param_get_data(&proto_data, buf, initial_buf_size);
        int buf_size = get_buf_size(buf, initial_buf_size);

        // allocate trimmed buffer and copy data
        uint8_t trimmed_buf[buf_size];
        trim_buffer(trimmed_buf, buf, buf_size);

        // find specific value from key in protodata
        ModuleConfig *unpacked_config = module_config__unpack(NULL, buf_size, trimmed_buf);
        const char *desired_key = "compressionRate";
        ConfigParameter *found_parameter = NULL;
        for (size_t i = 0; i < unpacked_config->n_parameters; i++)
        {
            if (strcmp(unpacked_config->parameters[i]->key, desired_key) == 0)
            {
                found_parameter = unpacked_config->parameters[i];
                break;
            }
        }

        // How to identify value type
        if (found_parameter != NULL)
        {
            switch (found_parameter->value_case)
            {
            case CONFIG_PARAMETER__VALUE_BOOL_VALUE:
                break;
            case CONFIG_PARAMETER__VALUE_FLOAT_VALUE:
                break;
            }
        }

        int paramValue = found_parameter->int_value;
        paramValue = param_get_uint8(params[i + 1]);
        int module_status = executeModuleInProcess(func, data, outputPipe, paramValue);

        if (module_status == FAILURE)
        {
            close(outputPipe[0]); // Close the read end of the pipe
            close(outputPipe[1]); // Close the write end of the pipe
            return i + 1;
        }

        size_t size;
        read(outputPipe[0], &size, sizeof(size)); // Read size from pipe
        ImageBatch result;
        read(outputPipe[0], &result, size); // Read the result from the pipe
        data->channels = result.channels;
        data->width = result.width;
        data->height = result.height;
        data->num_images = result.num_images;
        data->data_size = result.data_size;
        data->data = result.data;
    }

    close(outputPipe[0]); // Close the read end of the pipe
    close(outputPipe[1]); // Close the write end of the pipe

    return SUCCESS;
}

// Function to parse a configuration file with module and parameter names
int parseConfigFile(const char *configFile, char *modules[], int values[], int maxModules)
{
    FILE *file = fopen(configFile, "r");
    if (file == NULL)
    {
        fprintf(stderr, "Error: Unable to open the configuration file.\n");
        return 0; // Return 0 to indicate failure
    }

    int numModules = 0;
    char line[256]; // Adjust the buffer size as needed

    while (fgets(line, sizeof(line), file) != NULL && numModules < maxModules)
    {
        // Remove the newline character, if present
        size_t len = strlen(line);
        if (len > 0 && line[len - 1] == '\n')
        {
            line[len - 1] = '\0';
        }

        // Split the line by ':'
        char *moduleName = strtok(line, ":");
        char *paramName = strtok(NULL, ":");

        if (moduleName != NULL && paramName != NULL)
        {
            // Store the module and parameter names
            modules[numModules] = strdup(moduleName);
            values[numModules] = paramName[strlen(paramName) - 1] - '0';

            numModules++;
        }
    }

    fclose(file);

    return numModules;
}

// Function to load a module and parameter from a configuration file
void *loadModuleWithParam(const char *moduleName, const char *paramName)
{
    char filename[256]; // Adjust the buffer size as needed
    snprintf(filename, sizeof(filename), "./external_modules/%s.so", moduleName);

    // Load the external library dynamically
    void *handle = dlopen(filename, RTLD_LAZY);
    if (handle == NULL)
    {
        fprintf(stderr, "Error: Unable to load the library %s.\n", filename);
        return NULL;
    }

    // Get a function pointer to the external function
    const char *run = "run";
    void *functionPointer = dlsym(handle, run);
    if (functionPointer == NULL)
    {
        fprintf(stderr, "Error: Unable to find the function %s in %s.\n", run, filename);
        dlclose(handle);
        return NULL;
    }

    return functionPointer;
}

// Function to load modules from a configuration file
int loadModulesWithParams(const char *configFile, void *functionPointers[], char *modules[], int values[], int maxModules)
{
    int numModules = parseConfigFile(configFile, modules, values, maxModules);

    for (int i = 0; i < numModules; ++i)
    {
        // Load the module with the associated parameter
        functionPointers[i] = loadModuleWithParam(modules[i], values[i]);

        if (functionPointers[i] == NULL)
        {
            // Handle loading failure

            fprintf(stderr, "Error: Unable to load module %s with parameter %s.\n", modules[i], values[i]);
        }
    }

    return numModules;
}

void check_run(void)
{
    // run the pipeline if the _pipeline_run parameter is set w
    uint8_t do_run = param_get_uint8(&pipeline_run);
    if (do_run > 0)
    {
        run_pipeline();
        param_set_uint8(&pipeline_run, 0);
    }
}

void moduleConfigurations()
{
    // Module parameter config example (should be defined as a generic CSH method)
    ModuleConfig compression_config = MODULE_CONFIG__INIT;
    ConfigParameter compression_rate = CONFIG_PARAMETER__INIT;
    ConfigParameter compression_method = CONFIG_PARAMETER__INIT;
    compression_rate.key = "compressionRate";
    compression_rate.value_case = CONFIG_PARAMETER__VALUE_INT_VALUE;
    compression_rate.int_value = 1;
    compression_method.key = "method";
    compression_method.value_case = CONFIG_PARAMETER__VALUE_STRING_VALUE;
    compression_method.string_value = "JepbarComp";
    compression_config.parameters = malloc(sizeof(ConfigParameter *) * 2);
    compression_config.n_parameters = 2;
    compression_config.parameters[0] = &compression_rate;
    compression_config.parameters[1] = &compression_method;
    size_t lenConfig = module_config__get_packed_size(&compression_config);
    uint8_t bufConfig[lenConfig];
    module_config__pack(&compression_config, bufConfig);
    param_set_data(&proto_data, bufConfig, lenConfig);
}

void saveImage(const char *filename, const ImageBatch *batch)
{
    // Determine the desired output format (e.g., PNG)
    int stride = batch->width * batch->channels;
    int success = stbi_write_png(filename, batch->width, batch->height, batch->channels, batch->data, stride);
    if (!success)
    {
        fprintf(stderr, "Error writing image to %s\n", filename);
    }
    else
    {
        printf("Image saved as %s\n", filename);
    }
}

void run_pipeline(void)
{
    moduleConfigurations();

    int functionLimit = 10;
    void *functionPointers[functionLimit];
    char *modules[functionLimit]; // Array to store the module names
    int values[functionLimit];    // Array to store the parameters for the modules

    // Load modules from the configuration file
    int numModules = loadModulesWithParams("modules.txt", functionPointers, modules, values, functionLimit);

    // Initialize the pipeline
    Pipeline pipeline;
    initializePipeline(&pipeline, functionPointers, numModules);

    // Prepare the data
    ImageBatch data;
    data.mtype = 1;
    // data.height = 3;
    // data.width = 3;
    // data.channels = 1;
    // data.num_images = 1;
    // size_t data_size = data.height * data.width * data.channels * data.num_images;
    // data.data = (unsigned char *)malloc(data_size);
    // unsigned char img[] = {'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i'};
    // memcpy(data.data, img, data_size);
    // Populate the data array with image data
    const char *filename = "image.png"; // Change this to your image file
    int image_width, image_height, image_channels;
    unsigned char *image_data = stbi_load(filename, &image_width, &image_height, &image_channels, STBI_rgb_alpha);
    data.height = image_height;
    data.width = image_width;
    data.channels = image_channels;
    data.num_images = 1;
    size_t data_size = image_height * image_width * image_channels * 1;
    data.data_size = data_size;
    int shmid = shmget(1234, 1024 * 1024 * 10, IPC_CREAT | 0666);
    char *shmaddr = shmat(shmid, NULL, 0);
    data.data = shmaddr;
    memcpy(shmaddr, image_data, data_size); // Copy image batch data to shared memory

    // create msg queue
    int msg_queue_id;
    if ((msg_queue_id = msgget(68, 0666 | IPC_CREAT)) == -1)
    {
        perror("msgget error");
    }

    // send msg to queue
    if (msgsnd(msg_queue_id, &data, sizeof(data), 0) == -1)
    {
        perror("msgsnd error");
    }

    // recieve msg from queue
    ImageBatch datarcv;
    if (msgrcv(msg_queue_id, &datarcv, sizeof(data), 1, 0) == -1)
    {
        perror("msgrcv error");
    }

    // Execute the pipeline with parameter values
    int status = executePipeline(&pipeline, &datarcv, values);

    if (status != SUCCESS)
    {
        // Print failure message
        printf("Module named '%s' caused a failure in the pipeline\n", modules[status - 1]);
        return;
    }

    saveImage("output.png", &datarcv);

    // Print resulting data
    printf("Resulting data value: %c\n", datarcv.data[0]);

    // Clean up
    free(pipeline.functions);
}

#ifndef TYPES.H
#define TYPES.H

#include <stdlib.h>

typedef struct Data {
    int value;
} Data;

typedef int (*ProcessFunction)(Data *);

typedef struct {
    ProcessFunction* functions;
    size_t size;
} Pipeline;

#endif // TYPES.H
#include "greeting.h"

#include <stdio.h>

void greeting(const char *name, char *out, size_t size)
{
    snprintf(out, size, "Hello, %s!", name);
}

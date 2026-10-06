#ifndef GREETING_H
#define GREETING_H

#include <stddef.h>

// Writes "Hello, <name>!" into out (at most size bytes, always terminated).
void greeting(const char *name, char *out, size_t size);

#endif

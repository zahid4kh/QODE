#include "greeting.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    char out[64];
    greeting("world", out, sizeof out);
    assert(strcmp(out, "Hello, world!") == 0);

    greeting("a very long name that does not fit", out, 8);
    assert(strlen(out) == 7);

    puts("all tests passed");
    return 0;
}

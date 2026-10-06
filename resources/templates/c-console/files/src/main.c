#include "greeting.h"

#include <stdio.h>

int main(int argc, char *argv[])
{
    char line[256];
    greeting(argc > 1 ? argv[1] : "{{name}}", line, sizeof line);
    puts(line);
    return 0;
}

#include "greeting.h"

#include <iostream>

int main(int argc, char *argv[])
{
    const std::string who = argc > 1 ? argv[1] : "{{name}}";
    std::cout << greeting(who) << '\n';
    return 0;
}

#include "greeting.h"

#include <cassert>
#include <iostream>

int main()
{
    assert(greeting("world") == "Hello, world!");
    assert(greeting("").find("Hello") == 0);

    std::cout << "all tests passed\n";
    return 0;
}

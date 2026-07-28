#include <iostream>

#include <rlbs/core/version.hpp>

int main()
{
    std::cout
        << "Starting RLBS agent, version "
        << rlbs::version()
        << '\n';

    return 0;
}

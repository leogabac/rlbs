#include <iostream>

#include <rlbs/core/version.hpp>

int main()
{
    std::cout
        << "Starting "
        << rlbs::project_name()
        << " daemon, version "
        << rlbs::version()
        << '\n';

    return 0;
}

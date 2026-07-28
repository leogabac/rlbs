#include <iostream>

#include <rlbs/core/resources.hpp>
#include <rlbs/core/version.hpp>

int main() {

    // this syntax is called a designated initializer
    // easier to understand thatn node(16, 64000, 1)
    const rlbs::ResourceCapacity node{
        .cpus = 16,
        .memory_mb = 64000,
        .gpus = 1,
    };

    const rlbs::ResourceRequest job{
        .cpus = 8,
        .memory_mb = 16000,
        .gpus = 1,
    };

    std::cout << "Starting " << rlbs::project_name() << " daemon, version "
              << rlbs::version() << '\n';

    if (rlbs::can_fit(job, node)) {
        std::cout << "The example job fits on the example node.\n";
    } else {
        std::cout << "The example job does not fit on the example node.\n";
    }

    return 0;
}

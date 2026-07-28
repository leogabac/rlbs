#include <iostream>
#include <string_view>

#include <rlbs/core/version.hpp>

int main(int argc, char* argv[]) {
    if (argc > 1) {
        const std::string_view argument{argv[1]};

        if (argument == "--version") {
            std::cout << rlbs::project_name() << " " << rlbs::version() << '\n';

            return 0;
        }
    }

    std::cout << "rlbs: command-line client\n";
    return 0;
}

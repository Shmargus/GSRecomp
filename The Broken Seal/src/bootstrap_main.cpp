#include <iostream>
#include <string_view>

#ifndef GSR_EXPECTED_ROM_SHA1
#define GSR_EXPECTED_ROM_SHA1 "unknown"
#endif

int main(int argc, char** argv) {
    if (argc > 1 && std::string_view(argv[1]) == "--help") {
        std::cout
            << "GoldenSunRecomp starter bootstrap\n"
            << "This is not the game runner.\n"
            << "Expected ROM SHA-1: " << GSR_EXPECTED_ROM_SHA1 << "\n"
            << "See README.md and BUILDING.md to build the game.\n";
        return 0;
    }

    std::cout
        << "GoldenSunRecomp repository scaffold is buildable.\n"
        << "No ROM or BIOS was loaded.\n"
        << "See README.md and BUILDING.md to build the game.\n";
    return 0;
}

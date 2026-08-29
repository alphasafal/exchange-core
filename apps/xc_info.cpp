// Prints the toolchain the binaries were built with.
//
// The benchmark harness shells out to this so that a results table can never
// drift from the build that produced it.
#include <iostream>

#include "xc/util/build_info.hpp"

int main() {
    std::cout << xc::build_info().to_string();
    return 0;
}

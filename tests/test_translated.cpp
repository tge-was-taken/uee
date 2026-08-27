#include "autotest.h"

namespace uee {
bool runFunction(Machine& machine, u32 pc);
}

int main(int argc, char** argv) {
    return uee::autotest::run(argc, argv, uee::runFunction);
}

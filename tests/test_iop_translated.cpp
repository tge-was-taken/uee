#include "iop_autotest.h"

namespace uee {
bool runFunction(IopMachine& machine, u32 pc);
}

int main(int argc, char** argv) {
    return uee::autotest::runIop(argc, argv, uee::runFunction);
}

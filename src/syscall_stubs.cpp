#include "syscall_stubs.h"

#include <cstdio>
#include <set>
#include <string>

namespace uee {
namespace {

std::set<std::string>& reported() {
    static std::set<std::string> names;
    return names;
}

}  // namespace

void reportStub(const std::string& name) {
    if (reported().insert(name).second) {
        std::fprintf(stderr, "uee: %s is answered without the subsystem behind it\n",
                     name.c_str());
    }
}

}  // namespace uee

#pragma once

#include "types.h"

#include <string>

namespace uee {

// A call answered without the subsystem behind it. Each one says so the first
// time it is reached, so a shortcut is never silent, and stays quiet afterwards
// so a guest that loops over one does not bury the rest of the output.
//
// These are functions rather than arms of the dispatch switch because emitted
// code calls them directly: when the translator can see the syscall number at a
// call site it skips the guest's syscall instruction entirely.
void reportStub(const std::string& name);

}  // namespace uee

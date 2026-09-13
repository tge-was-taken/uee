#pragma once

#include "translator.h"
#include "vu_programs.h"

#include <string_view>

namespace uee {

// Every start the log recorded, one C++ function each, plus the table the
// runtime looks them up in.
TranslatedSource translateVuPrograms(const VuProgramLog& log, usize units,
                                     std::string_view declarationsHeader);

}  // namespace uee

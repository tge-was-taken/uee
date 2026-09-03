// SLUS_216.21 md5 9074ba25f5a100940ed233c9f8f675ab names these.

#include "movie.h"

#include <span>

#include "ee_context.h"
#include "ipu.h"
#include "log.h"

namespace uee {
namespace {

LoggerChannel log("movie");

constexpr u32 kResultRegister = 2;
constexpr u32 kReturnRegister = 31;

// _getpic @ 0x500278 answers this once it has a picture.
constexpr u32 kDecoded = 1;

}  // namespace

bool Movie::enter(EeContext& ee) {
    R5900& cpu = ee.cpu;
    cpu.pc = static_cast<u32>(cpu.gpr[kReturnRegister].lo);
    cpu.write64(kResultRegister, kDecoded);

    const std::span<const u8> stream = mIpu.getStream();
    log.trace("picture asked for, {} bytes to the host", stream.size());
    mVideo->write(stream);
    mIpu.consume(stream.size());
    return true;
}

}  // namespace uee

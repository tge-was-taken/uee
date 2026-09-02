// SLUS_216.21 md5 9074ba25f5a100940ed233c9f8f675ab names these.

#include "movie.h"

#include "ipu.h"
#include "log.h"
#include "memory.h"
#include "ee_context.h"

namespace uee {
namespace {

LoggerChannel log("movie");

constexpr u32 kPictureRegister = 5;
constexpr u32 kBlockRegister = 6;
constexpr u32 kResultRegister = 2;
constexpr u32 kReturnRegister = 31;

constexpr u32 kBlockSide = 16;
constexpr u32 kBlockPixels = kBlockSide * kBlockSide;
constexpr u32 kPixelBytes = 4;

// _getpic @ 0x500278 answers this once it has a picture.
constexpr u32 kDecoded = 1;

// TextureDecoder::decodeMpegFrame in the PC port reads a picture back at this
// stride.
u32 wordOf(u32 x, u32 y, u32 blockRows) {
    const u32 block = x / kBlockSide * blockRows + y / kBlockSide;
    return block * kBlockPixels + (y % kBlockSide) * kBlockSide + x % kBlockSide;
}

}  // namespace

bool Movie::enter(EeContext& ee) {
    R5900& cpu = ee.cpu;
    cpu.pc = static_cast<u32>(cpu.gpr[kReturnRegister].lo);
    cpu.write64(kResultRegister, kDecoded);

    mDecoder->feed(mIpu.getStream());
    mIpu.consume(mIpu.getStream().size());

    const std::optional<Picture> picture = mDecoder->takePicture();
    if (!picture) {
        return true;
    }

    // SFMPVP_DecodeFrmSj @ 0x595390 asks for the macroblocks the picture covers,
    // so another size is another stream.
    const u32 blockRows = picture->height / kBlockSide;
    const u32 blocks = picture->width / kBlockSide * blockRows;
    const u32 wanted = static_cast<u32>(cpu.gpr[kBlockRegister].lo);
    if (blocks != wanted) {
        log.error("a {}x{} picture covers {} macroblocks where {} were asked for",
                  picture->width, picture->height, blocks, wanted);
        return true;
    }

    const u32 to = static_cast<u32>(cpu.gpr[kPictureRegister].lo);
    log.trace("picture {}x{} to {:08x}", picture->width, picture->height, to);
    for (u32 y = 0; y < picture->height; ++y) {
        for (u32 x = 0; x < picture->width; ++x) {
            const usize from = (static_cast<usize>(y) * picture->width + x) * kPixelBytes;
            mMemory.write32(to + wordOf(x, y, blockRows) * kPixelBytes,
                            static_cast<u32>(picture->rgba[from]) |
                                static_cast<u32>(picture->rgba[from + 1]) << 8 |
                                static_cast<u32>(picture->rgba[from + 2]) << 16 |
                                static_cast<u32>(picture->rgba[from + 3]) << 24);
        }
    }
    return true;
}

}  // namespace uee

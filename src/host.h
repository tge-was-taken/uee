#pragma once

#include "types.h"

#include <optional>
#include <span>

namespace uee {

// What the sound processor plays at, so anything writing these samples out says
// the rate and width the guest produced them at.
constexpr u32 kSpuSampleRate = 48'000;
constexpr u16 kSpuChannels = 2;

class AudioSink {
public:
    virtual ~AudioSink() = default;

    virtual void write(std::span<const s16> samples) = 0;
};

// A memory card image, in the pages the card protocol moves. A driver reads
// these off the card, so they are what it will expect to find.
constexpr u32 kCardPageBytes = 512;
constexpr u32 kCardSpareBytes = 16;
constexpr u32 kCardPagesPerBlock = 16;
constexpr u32 kCardPages = 16384;

class CardStorage {
public:
    virtual ~CardStorage() = default;

    virtual void readPage(u32 page, std::span<u8> out) = 0;
    virtual void writePage(u32 page, std::span<const u8> in) = 0;
};

// Borrowed until the next picture is taken.
struct Picture {
    std::span<const u8> rgba;
    u32 width = 0;
    u32 height = 0;
};

// How a picture then sits in guest memory is the runtime's, not a decoder's.
class VideoDecoder {
public:
    virtual ~VideoDecoder() = default;

    virtual void feed(std::span<const u8> stream) = 0;

    // Nothing until a whole picture has arrived.
    virtual std::optional<Picture> takePicture() = 0;
};

class PadSource {
public:
    virtual ~PadSource() = default;

    // 0 = active so nothing pressed is all ones.
    virtual u16 buttons() = 0;
};

}  // namespace uee

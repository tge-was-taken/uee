#pragma once

#include "types.h"

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

// The compressed video the IPU was handed, once per picture the guest asks for.
// Nothing here decodes it.
class VideoSink {
public:
    virtual ~VideoSink() = default;

    virtual void write(std::span<const u8> stream) = 0;
};

// The graphics stream as the interface hands it on, one burst per path it came
// down, and the privileged registers as they stood at the field. Nothing here
// draws.
class GsHandler {
public:
    virtual ~GsHandler() = default;

    // Said once, before any packet: what the registers hold is all a reader
    // knows of the state the stream is about to change.
    virtual void begin(std::span<const u8> privilegedRegisters) = 0;

    virtual void transfer(u32 path, std::span<const u8> data) = 0;
    virtual void vsync(u32 field, std::span<const u8> privilegedRegisters) = 0;
};

// The buttons as libpad reports them, the poll response's first data byte in the
// high half.
constexpr u16 kPadLeft = 0x8000;
constexpr u16 kPadDown = 0x4000;
constexpr u16 kPadRight = 0x2000;
constexpr u16 kPadUp = 0x1000;
constexpr u16 kPadStart = 0x0800;
constexpr u16 kPadR3 = 0x0400;
constexpr u16 kPadL3 = 0x0200;
constexpr u16 kPadSelect = 0x0100;
constexpr u16 kPadSquare = 0x0080;
constexpr u16 kPadCross = 0x0040;
constexpr u16 kPadCircle = 0x0020;
constexpr u16 kPadTriangle = 0x0010;
constexpr u16 kPadR1 = 0x0008;
constexpr u16 kPadL1 = 0x0004;
constexpr u16 kPadR2 = 0x0002;
constexpr u16 kPadL2 = 0x0001;

class PadSource {
public:
    virtual ~PadSource() = default;

    // 0 = active so nothing pressed is all ones.
    virtual u16 buttons() = 0;
};

}  // namespace uee

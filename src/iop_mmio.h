#pragma once

#include "adpcm.h"
#include "host.h"
#include "memory.h"
#include "sio2.h"
#include "mmio.h"
#include "types.h"

#include <optional>

namespace uee {

class IopKernel;

class IopMmio : public Mmio {
public:
    void attach(Memory& memory, IopKernel& kernel);
    void attachAudio(AudioSink* audio) { mAudio = audio; }
    void attachPad(PadSource* pad) { mSio2.attachPad(pad); }

    std::span<const MmioSegment> segments() const override {
        static constexpr MmioSegment kSegments[] = {{kIopHardwareBase, kIopHardwareSize},
                                                  {kSpu2Base, kSpu2Size}};
        return kSegments;
    }

    std::optional<u32> readWord(u32 address) const override;
    bool writeWord(u32 address, u32 value) override;
    std::optional<u16> readHalf(u32 address) const override;
    bool writeHalf(u32 address, u16 value) override;

private:
    // LIBSD md5 7e79d40b99f8d87a5084188956550cd5 drives one channel per SPU2
    // core, and registers a handler on each channel's interrupt line.
    struct SpuChannel {
        u32 base = 0;
        u32 line = 0;
        u32 madr = 0;
        u32 bcr = 0;
        u32 chcr = 0;
    };

    void sendToAudio(const SpuChannel& channel, u32 words);

    SpuChannel* channelFor(u32 address);
    const SpuChannel* channelFor(u32 address) const;

    Sio2 mSio2;
    AudioSink* mAudio = nullptr;
    AdpcmState mAdpcm;
    Memory* mMemory = nullptr;
    IopKernel* mKernel = nullptr;
    SpuChannel mChannels[2] = {{0x1F8010C0, 36, 0, 0, 0}, {0x1F801500, 40, 0, 0, 0}};
};

}  // namespace uee

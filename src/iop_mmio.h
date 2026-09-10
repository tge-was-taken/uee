#pragma once

#include "host.h"
#include "memory.h"
#include "sio2.h"
#include "spu2.h"
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
    void attachCard(CardStorage* card) { mSio2.attachCard(card); }

    // The mixer runs off the same clock the transfers are paced by.
    void advance(u64 cycles);

    std::span<const MmioSegment> segments() const override {
        static constexpr MmioSegment kSegments[] = {{kIopHardwareBase, kIopHardwareSize},
                                                  {kSpu2Base, kSpu2Size}};
        return kSegments;
    }

    void startVu1(u32 address) override;

    std::optional<u32> readWord(u32 address) override;
    bool writeWord(u32 address, u32 value) override;
    std::optional<u16> readHalf(u32 address) override;
    bool writeHalf(u32 address, u16 value) override;
    std::optional<u8> readByte(u32 address) override;
    bool writeByte(u32 address, u8 value) override;

private:
    struct DmaChannel {
        u32 base = 0;
        u32 line = 0;
        u32 madr = 0;
        u32 bcr = 0;
        u32 chcr = 0;
    };

    void startTransfer(DmaChannel& channel);
    void sendToSoundMemory(const DmaChannel& channel, u32 words);
    void writeToSio2(const DmaChannel& channel);
    void readFromSio2();

    DmaChannel* channelFor(u32 address);
    const DmaChannel* channelFor(u32 address) const;

    Sio2 mSio2;
    Spu2 mSpu2;
    AudioSink* mAudio = nullptr;
    u64 mAudioCycles = 0;
    Memory* mMemory = nullptr;
    IopKernel* mKernel = nullptr;

    // LIBSD drives one channel per SPU2 core and waits on its interrupt.
    DmaChannel mSpuChannels[2] = {{0x1F8010C0, 36}, {0x1F801500, 40}};

    // SIO2MAN waits on the SIO2 interrupt instead, so these carry no line.
    DmaChannel mSio2Write{0x1F801540};
    DmaChannel mSio2Read{0x1F801550};
};

}  // namespace uee

#pragma once

#include "hook.h"
#include "host.h"
#include "types.h"

namespace uee {

class Ipu;
class Memory;

// Answers sceMpegGetPicture, since nothing here drives the IPU.
class Movie final : public Hook {
public:
    Movie(Memory& memory, Ipu& ipu, u32 entry) : mMemory(memory), mIpu(ipu), mEntry(entry) {}

    void attach(VideoDecoder* decoder) { mDecoder = decoder; }

    bool isHooked(u32 pc) const override {
        return mEntry != 0 && pc == mEntry && mDecoder != nullptr;
    }

    bool enter(EeContext& ee) override;

private:
    Memory& mMemory;
    Ipu& mIpu;
    VideoDecoder* mDecoder = nullptr;
    u32 mEntry = 0;
};

}  // namespace uee

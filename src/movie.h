#pragma once

#include "hook.h"
#include "host.h"
#include "types.h"

namespace uee {

class Ipu;

// Answers sceMpegGetPicture, since nothing here drives the IPU. The picture is
// the host's to make: the guest's own buffer is left as it was.
class Movie final : public Hook {
public:
    Movie(Ipu& ipu, u32 entry) : mIpu(ipu), mEntry(entry) {}

    void attach(VideoSink* video) { mVideo = video; }

    bool isHooked(u32 pc) const override {
        return mEntry != 0 && pc == mEntry && mVideo != nullptr;
    }

    bool enter(EeContext& ee) override;

private:
    Ipu& mIpu;
    VideoSink* mVideo = nullptr;
    u32 mEntry = 0;
};

}  // namespace uee

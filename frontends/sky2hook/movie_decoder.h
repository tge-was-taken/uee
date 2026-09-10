#pragma once

#include "host.h"

#include <memory>
#include <span>
#include <vector>

struct plm_buffer_t;
struct plm_video_t;

namespace sky2 {

// The MPEG-1 the disc carries, decoded straight into the renderer. Nothing the
// guest holds is involved.
class MovieDecoder final : public uee::VideoSink {
public:
    MovieDecoder();
    ~MovieDecoder() override;

    void write(std::span<const u8> stream) override;

private:
    struct Free {
        void operator()(plm_buffer_t* buffer) const;
        void operator()(plm_video_t* video) const;
    };

    std::unique_ptr<plm_buffer_t, Free> mBuffer;
    std::unique_ptr<plm_video_t, Free> mVideo;
    std::vector<u8> mRgba;
};

}  // namespace sky2

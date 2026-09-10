#include "movie_decoder.h"

// pl_mpeg names size_t and FILE in its declarations but only includes their
// headers with its implementation.
#include <stddef.h>
#include <stdio.h>

#include <pl_mpeg.h>

#include "skygl_hooks.h"

namespace sky2 {

void MovieDecoder::Free::operator()(plm_buffer_t* buffer) const {
    plm_buffer_destroy(buffer);
}

void MovieDecoder::Free::operator()(plm_video_t* video) const {
    plm_video_destroy(video);
}

// The decoder buffers, so what the IPU was fed goes over whole and finding the
// picture boundaries in it is its problem rather than the caller's.
MovieDecoder::MovieDecoder()
    : mBuffer(plm_buffer_create_with_capacity(PLM_BUFFER_DEFAULT_SIZE)),
      mVideo(plm_video_create_with_buffer(mBuffer.get(), 0)) {}

MovieDecoder::~MovieDecoder() = default;

void MovieDecoder::write(std::span<const u8> stream) {
    plm_buffer_write(mBuffer.get(), const_cast<u8*>(stream.data()), stream.size());

    plm_frame_t* frame = plm_video_decode(mVideo.get());
    if (frame == nullptr) {
        return;
    }
    // The conversion writes three bytes per pixel and never the fourth, so the
    // alpha has to be there already or the frame draws fully transparent.
    mRgba.assign(static_cast<usize>(frame->width) * frame->height * 4, 0xFF);
    plm_frame_to_rgba(frame, mRgba.data(), static_cast<int>(frame->width * 4));
    skygl::hooks::setMoviePixels(mRgba.data(), static_cast<int>(frame->width),
                                 static_cast<int>(frame->height));
}

}  // namespace sky2

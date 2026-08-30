#pragma once

#include "host.h"
#include "types.h"

#include <cstdio>
#include <string>
#include <vector>

namespace uee {

class WavSink : public AudioSink {
public:
    WavSink(const std::string& path, u32 sampleRate, u16 channels)
        : mFile(std::fopen(path.c_str(), "wb")), mSampleRate(sampleRate), mChannels(channels) {
        if (mFile != nullptr) {
            const std::vector<u8> header(kHeaderBytes, 0);
            std::fwrite(header.data(), 1, header.size(), mFile);
        }
    }

    ~WavSink() override {
        if (mFile == nullptr) {
            return;
        }
        writeHeader();
        std::fclose(mFile);
    }

    WavSink(const WavSink&) = delete;
    WavSink& operator=(const WavSink&) = delete;

    bool isOpen() const { return mFile != nullptr; }

    void write(std::span<const s16> samples) override {
        if (mFile == nullptr) {
            return;
        }
        std::fwrite(samples.data(), sizeof(s16), samples.size(), mFile);
        mSamples += samples.size();
    }

private:
    static constexpr usize kHeaderBytes = 44;
    static constexpr u16 kBitsPerSample = 16;

    void put(std::vector<u8>& out, u32 value, usize bytes) {
        for (usize i = 0; i < bytes; ++i) {
            out.push_back(static_cast<u8>(value >> (8 * i)));
        }
    }

    void writeHeader() {
        const u32 dataBytes = static_cast<u32>(mSamples * sizeof(s16));
        std::vector<u8> header;
        header.insert(header.end(), {'R', 'I', 'F', 'F'});
        put(header, 36 + dataBytes, 4);
        header.insert(header.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
        put(header, 16, 4);
        put(header, 1, 2);
        put(header, mChannels, 2);
        put(header, mSampleRate, 4);
        put(header, mSampleRate * mChannels * kBitsPerSample / 8, 4);
        put(header, mChannels * kBitsPerSample / 8, 2);
        put(header, kBitsPerSample, 2);
        header.insert(header.end(), {'d', 'a', 't', 'a'});
        put(header, dataBytes, 4);

        std::fseek(mFile, 0, SEEK_SET);
        std::fwrite(header.data(), 1, header.size(), mFile);
    }

    std::FILE* mFile = nullptr;
    u32 mSampleRate = 0;
    u16 mChannels = 0;
    usize mSamples = 0;
};

}  // namespace uee

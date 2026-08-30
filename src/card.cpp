// An `Mc2*` tag names the routine that settles a detail, in MC2_S1
// md5 c493ecb797ddd06b4ebd7eee6aff1959.

#include "card.h"

#include <algorithm>
#include <vector>

namespace uee {
namespace {

// The commands Mc2PackProbe @ 0xa20 and its neighbours send.
constexpr u8 kProbe = 0x11;
constexpr u8 kSetErasePage = 0x21;
constexpr u8 kSetWritePage = 0x22;
constexpr u8 kSetReadPage = 0x23;
constexpr u8 kWriteData = 0x42;
constexpr u8 kReadData = 0x43;
constexpr u8 kGetSpecs = 0x26;
constexpr u8 kSetTerminator = 0x27;
constexpr u8 kGetTerminator = 0x28;
constexpr u8 kMagicGate = 0xF3;

constexpr u8 kIdle = 0xFF;
constexpr u8 kAck = 0x2B;
constexpr usize kAckIndex = 2;
constexpr usize kPayloadIndex = 3;

// Mc2PageAddressPack @ 0x193c checks an address with the same xor.
constexpr usize kAddressBytes = 4;

u8 xorOf(std::span<const u8> bytes) {
    u8 sum = 0;
    for (const u8 byte : bytes) {
        sum ^= byte;
    }
    return sum;
}

void put(std::span<u8> data, std::initializer_list<u8> bytes) {
    usize i = 0;
    for (const u8 byte : bytes) {
        if (i < data.size()) {
            data[i++] = byte;
        }
    }
}

}  // namespace

void MemoryCard::answer(std::span<const u8> command, std::span<u8> response) {
    std::fill(response.begin(), response.end(), kIdle);
    if (command.size() < 2 || response.size() < kPayloadIndex + 1) {
        return;
    }
    response[kAckIndex] = kAck;
    response.back() = mTerminator;

    const std::span<const u8> payload = command.subspan(2);
    const std::span<u8> data = response.subspan(kPayloadIndex, response.size() - kPayloadIndex - 1);

    switch (command[1]) {
        case kGetSpecs: reportSpecs(data); return;
        case kGetTerminator: reportTerminator(data); return;
        case kSetTerminator:
            setTerminator(payload.empty() ? mTerminator : payload[0]);
            return;
        case kSetReadPage:
        case kSetWritePage:
        case kSetErasePage: setPage(payload); return;
        case kReadData: readData(data, payload.empty() ? 0 : payload[0]); return;
        case kWriteData: writeData(payload, payload.empty() ? 0 : payload[0]); return;
        // MC2_S1: Mc2SendCmdF3 @ 0x934 wants an ack where every other command
        // has its first answer byte.
        case kMagicGate: put(data, {kAck}); return;
        case kProbe:
        default: return;
    }
}

// Mc2GetSpecs @ 0x14cc checks the xor of these eight bytes against the one
// after them.
void MemoryCard::reportSpecs(std::span<u8> data) {
    const u8 specs[] = {static_cast<u8>(kCardPageBytes),
                        static_cast<u8>(kCardPageBytes >> 8),
                        static_cast<u8>(kCardPagesPerBlock),
                        static_cast<u8>(kCardPagesPerBlock >> 8),
                        static_cast<u8>(kCardPages),
                        static_cast<u8>(kCardPages >> 8),
                        static_cast<u8>(kCardPages >> 16),
                        static_cast<u8>(kCardPages >> 24)};
    for (usize i = 0; i < std::size(specs) && i < data.size(); ++i) {
        data[i] = specs[i];
    }
    if (data.size() > std::size(specs)) {
        data[std::size(specs)] = xorOf(specs);
    }
}

// Mc2GetTerminator @ 0x176c takes a card whose answer disagrees with the
// terminator that follows it as the older kind.
void MemoryCard::reportTerminator(std::span<u8> data) {
    put(data, {mTerminator});
}

void MemoryCard::setTerminator(u8 value) {
    mTerminator = value;
}

// Mc2PackSetReadPage @ 0xf98 sends the page number little endian, xor last.
void MemoryCard::setPage(std::span<const u8> payload) {
    if (payload.size() < kAddressBytes) {
        return;
    }
    mPage = static_cast<u32>(payload[0]) | static_cast<u32>(payload[1]) << 8 |
            static_cast<u32>(payload[2]) << 16 | static_cast<u32>(payload[3]) << 24;
    mOffset = 0;
}

// Mc2ReadPage @ 0xdb8 takes the answer 128 bytes at a time, xor last.
void MemoryCard::readData(std::span<u8> data, usize count) {
    if (data.empty()) {
        return;
    }
    data[0] = kAck;
    std::vector<u8> page(kCardPageBytes + kCardSpareBytes);
    mStorage->readPage(mPage, page);

    const std::span<u8> out = data.subspan(1);
    for (usize i = 0; i < count && i < out.size(); ++i) {
        const usize at = mOffset + i;
        out[i] = at < page.size() ? page[at] : 0;
    }
    if (count < out.size()) {
        out[count] = xorOf(out.subspan(0, count));
    }
    mOffset += count;
}

// Mc2PackWriteData @ 0x1314 sends the bytes, xor last.
void MemoryCard::writeData(std::span<const u8> payload, usize count) {
    if (payload.size() < count + 1) {
        return;
    }
    std::vector<u8> page(kCardPageBytes + kCardSpareBytes);
    mStorage->readPage(mPage, page);
    for (usize i = 0; i < count && mOffset + i < page.size(); ++i) {
        page[mOffset + i] = payload[i + 1];
    }
    mStorage->writePage(mPage, page);
    mOffset += count;
}

}  // namespace uee

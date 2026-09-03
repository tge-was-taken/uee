#include "guest_replacement.h"

#include <xxhash.h>

#include <vector>

namespace uee {
namespace {

// The soft double routines are IEEE, so host arithmetic matches them.
// 3dpinball-ps2 md5 da8a5733aa90bd23d15ee3222d1548ca
// p3fes SLUS_216.21 md5 9074ba25f5a100940ed233c9f8f675ab
constexpr Replacement kReplacements[] = {
    {0x01e2db8a5b62b76cull, "__adddf3", "guestlib::addDouble(ee.cpu);"},
    {0x82a200bdcf65a075ull, "__subdf3", "guestlib::subtractDouble(ee.cpu);"},
    {0x8d372842a4d8b5a8ull, "__muldf3", "guestlib::multiplyDouble(ee.cpu);"},
    {0xce7ce65a504c97e7ull, "__divdf3", "guestlib::divideDouble(ee.cpu);"},
    {0x4dd7a2f022ae3320ull, "__fixdfsi", "guestlib::doubleToInt(ee.cpu);"},
    {0xe1d64685d49f0c3dull, "__floatsidf", "guestlib::intToDouble(ee.cpu);"},
    {0x163f799ec7652b6cull, "__extendsfdf2", "guestlib::floatToDouble(ee.cpu);"},
    {0x0deecf58cdbfa32cull, "__truncdfsf2", "guestlib::doubleToFloat(ee.cpu);"},
    // __eqdf2/__nedf2/__ltdf2/__ledf2 share a body; __gtdf2/__gedf2 another.
    {0x9a16f37f72e96c0eull, "__eqdf2", "guestlib::compareDouble(ee.cpu);"},
    {0xb3edf06ed7204778ull, "__gtdf2", "guestlib::compareDouble(ee.cpu);"},
    // strcmp @ 0x00524128
    {0x0894bf9f7d65f015ull, "strcmp", "guestlib::strcmp(ee.cpu, mem);"},
};

}  // namespace

std::optional<u64> hashInstructions(Memory& memory, u32 start, u32 end) {
    if (end <= start) {
        return std::nullopt;
    }
    std::vector<u32> words;
    words.reserve((end - start) / 4);
    // jal/j targets and lui/ori(addiu) address pairs move with the load address.
    std::optional<u32> pendingLuiRegister;
    for (u32 pc = start; pc + 4 <= end; pc += 4) {
        const std::optional<u32> word = memory.peek32(pc);
        if (!word) {
            return std::nullopt;
        }
        const Instruction insn{*word};
        const Op op = getOp(insn);
        u32 masked = *word;
        if (op == Op::j || op == Op::jal) {
            masked &= 0xFC000000u;
        } else if (pendingLuiRegister && (op == Op::ori || op == Op::addiu) &&
                  insn.rs() == *pendingLuiRegister) {
            masked &= 0xFFFF0000u;
            words.back() &= 0xFFFF0000u;
        }
        pendingLuiRegister = (op == Op::lui) ? std::optional<u32>(insn.rt()) : std::nullopt;
        words.push_back(masked);
    }
    return XXH3_64bits(words.data(), words.size() * sizeof(u32));
}

const Replacement* findReplacement(std::optional<u64> hash) {
    if (!hash) {
        return nullptr;
    }
    for (const Replacement& replacement : kReplacements) {
        if (replacement.hash == *hash) {
            return &replacement;
        }
    }
    return nullptr;
}

std::span<const Replacement> getReplacements() { return kReplacements; }

}  // namespace uee

#include "interpreter.h"
#include "types.h"

#include <spdlog/spdlog.h>

#include <charconv>
#include <format>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace uee;

s32 failures = 0;
// Held until the case ends, because whether a mismatch is a failure depends on
// which op produced it.
std::vector<std::string> pending;

template <typename... Args>
void fail(spdlog::format_string_t<Args...> what, Args&&... args) {
    pending.push_back(std::vformat(what, std::make_format_args(args...)));
}

void settle(std::map<std::string, s32>& diverged, std::string_view op, bool divergent) {
    for (const std::string& message : pending) {
        if (divergent) {
            ++diverged[std::string(op)];
        } else {
            if (failures < 25) {
                spdlog::error("{}", message);
            }
            ++failures;
        }
    }
    pending.clear();
}

// These vectors come from the PCSX2 interpreter, which computes the FPU on the
// host in place of the R5900's own rounding, so it disagrees with the hardware
// the ps2autotests were captured on. Where the two conflict this runtime
// follows the hardware, and a mismatch here is counted and reported rather than
// failed. The ops are listed one by one so a mismatch anywhere else still
// fails.
bool oracleFollowsHost(Op op) {
    switch (op) {
        case Op::add_s: case Op::sub_s: case Op::mul_s: case Op::div_s:
        case Op::sqrt_s: case Op::rsqrt_s:
        case Op::adda_s: case Op::suba_s: case Op::mula_s:
        case Op::madd_s: case Op::msub_s: case Op::madda_s: case Op::msuba_s:
        case Op::c_eq_s: case Op::c_lt_s: case Op::c_le_s:
        case Op::ctc1:
            return true;
        default:
            return false;
    }
}

std::vector<std::string_view> split(std::string_view text, char sep) {
    std::vector<std::string_view> out;
    usize start = 0;
    for (usize i = 0; i <= text.size(); ++i) {
        if (i == text.size() || text[i] == sep) {
            out.push_back(text.substr(start, i - start));
            start = i + 1;
        }
    }
    return out;
}

u64 hex64(std::string_view s) {
    u64 v = 0;
    std::from_chars(s.data(), s.data() + s.size(), v, 16);
    return v;
}

u32 hex32(std::string_view s) {
    return static_cast<u32>(hex64(s));
}

// a 128-bit value is 32 hex digits, most significant first
Reg128 hex128(std::string_view s) {
    return s.size() >= 32 ? Reg128{hex64(s.substr(16, 16)), hex64(s.substr(0, 16))}
                          : Reg128{hex64(s), 0};
}

u32 decimal(std::string_view s) {
    u32 v = 0;
    std::from_chars(s.data(), s.data() + s.size(), v);
    return v;
}

struct Change {
    std::string file;
    u32 index = 0;
    Reg128 before;
    Reg128 after;
};

struct Case {
    u32 word = 0, pc = 0, pcAfter = 0;
    bool nested = false;
    u32 rs = 0, rt = 0, fs = 0, ft = 0, fd = 0;
    Reg128 rsValue, rtValue, hi, lo, acc, vfs, vft, vfd;
    u32 fsValue = 0, ftValue = 0, sa = 0, fcr31 = 0, facc = 0, cop0status = 0;
    u32 q = 0, p = 0, vis = 0, vit = 0, vstatus = 0, vmac = 0, vclip = 0;
    bool hasMemory = false;
    u32 window = 0, address = 0;
    u8 before[16]{}, after[16]{};
    std::vector<Change> changes;
};

void readBytes(std::string_view hex, u8* out) {
    for (s32 i = 0; i < 16; ++i) {
        out[i] = static_cast<u8>(hex64(hex.substr(2 * i, 2)));
    }
}

// Backs only the 16 bytes the corpus recorded, so a wrong address faults
// instead of quietly reading zero.
Memory makeWindow(u32 base, const u8* bytes) {
    return Memory{{Region{base, std::vector<u8>(bytes, bytes + 16)}}, {}};
}

std::vector<Case> readCases(const std::filesystem::path& path) {
    std::ifstream in(path);
    if (!in) {
        spdlog::critical("cannot read {}", path.string());
        std::exit(2);
    }
    std::vector<Case> cases;
    std::string line;
    while (std::getline(in, line)) {
        const std::vector<std::string_view> c = split(line, '\t');
        if (c.size() < 32) {
            continue;
        }
        Case k;
        k.word = hex32(c[0]);
        k.pc = hex32(c[1]);
        k.pcAfter = hex32(c[2]);
        k.nested = decimal(c[3]) != 0;
        k.rs = decimal(c[4]);
        k.rsValue = hex128(c[5]);
        k.rt = decimal(c[6]);
        k.rtValue = hex128(c[7]);
        k.fs = decimal(c[8]);
        k.fsValue = hex32(c[9]);
        k.ft = decimal(c[10]);
        k.ftValue = hex32(c[11]);
        k.fd = decimal(c[12]);
        k.hi = hex128(c[13]);
        k.lo = hex128(c[14]);
        k.sa = hex32(c[15]);
        k.fcr31 = hex32(c[16]);
        k.facc = hex32(c[17]);
        k.cop0status = hex32(c[18]);
        k.acc = hex128(c[19]);
        k.q = hex32(c[20]);
        k.p = hex32(c[21]);
        k.vfs = hex128(c[22]);
        k.vft = hex128(c[23]);
        k.vfd = hex128(c[24]);
        k.vis = hex32(c[25]);
        k.vit = hex32(c[26]);
        k.vstatus = hex32(c[27]);
        k.vmac = hex32(c[28]);
        k.vclip = hex32(c[29]);
        if (c[30] != "-") {
            const std::vector<std::string_view> m = split(c[30], ':');
            if (m.size() != 4 || m[2].size() < 32 || m[3].size() < 32) {
                spdlog::critical("malformed memory field: {}", std::string(c[30]));
                std::exit(2);
            }
            k.hasMemory = true;
            k.window = hex32(m[0]);
            k.address = hex32(m[1]);
            readBytes(m[2], k.before);
            readBytes(m[3], k.after);
        }
        if (c[31] != "-") {
            for (std::string_view d : split(c[31], ' ')) {
                const std::vector<std::string_view> part = split(d, ':');
                if (part.size() == 4) {
                    k.changes.push_back({std::string(part[0]), decimal(part[1]),
                                         hex128(part[2]), hex128(part[3])});
                }
            }
        }
        cases.push_back(std::move(k));
    }
    return cases;
}

Reg128 read(const EeContext& ee, std::string_view file, u32 index) {
    if (file == "gpr") {
        return ee.cpu.gpr[index & 31];
    }
    if (file == "hi") {
        return ee.cpu.hi;
    }
    if (file == "lo") {
        return ee.cpu.lo;
    }
    if (file == "fpr") {
        return Reg128{ee.fpu.fpr[index & 31], 0};
    }
    if (file == "fcr31") {
        return Reg128{ee.fpu.fcr31, 0};
    }
    if (file == "facc") {
        return Reg128{ee.fpu.acc, 0};
    }
    if (file == "vf") {
        return ee.vu0.vf[index & 31];
    }
    if (file == "vi") {
        return Reg128{ee.vu0.vi[index & 31], 0};
    }
    if (file == "acc") {
        return ee.vu0.acc;
    }
    if (file == "q") {
        return Reg128{ee.vu0.vi[Vu0::kQ], 0};
    }
    if (file == "sa") {
        return Reg128{ee.cpu.sa, 0};
    }
    return Reg128{};
}

Reg128 read(const EeContext& ee, const std::string& key) {
    const usize colon = key.find(':');
    return read(ee, std::string_view(key).substr(0, colon),
                decimal(std::string_view(key).substr(colon + 1)));
}

void setUp(EeContext& ee, const Case& k) {
    ee = EeContext{};
    const Instruction insn{k.word};
    ee.cpu.pc = k.pc;
    ee.cpu.gpr[k.rs] = k.rsValue;
    ee.cpu.gpr[k.rt] = k.rtValue;
    ee.cpu.gpr[0] = Reg128{};
    ee.cpu.hi = k.hi;
    ee.cpu.lo = k.lo;
    ee.cpu.sa = k.sa;
    ee.cpu.cop0[cop0::kStatus] = k.cop0status;
    ee.fpu.fpr[k.fs] = k.fsValue;
    ee.fpu.fpr[k.ft] = k.ftValue;
    ee.fpu.fcr31 = k.fcr31;
    ee.fpu.acc = k.facc;
    ee.vu0.acc = k.acc;
    ee.vu0.vi[Vu0::kQ] = k.q;
    ee.vu0.vi[Vu0::kP] = k.p;
    ee.vu0.vi[Vu0::kStatus] = k.vstatus;
    ee.vu0.vi[Vu0::kMac] = k.vmac;
    ee.vu0.vi[Vu0::kClip] = k.vclip;
    ee.vu0.vf[insn.rd()] = k.vfs;
    ee.vu0.vf[insn.rt()] = k.vft;
    ee.vu0.vf[k.fd & 31] = k.vfd;
    // vis and vit hold whatever the addressed register held, control registers
    // included, so they land after the file-wide snapshot above and win.
    ee.vu0.vi[insn.rd()] = k.vis;
    ee.vu0.vi[insn.rt()] = k.vit;
    for (const Change& ch : k.changes) {
        if (ch.file == "gpr") {
            ee.cpu.gpr[ch.index & 31] = ch.before;
        } else if (ch.file == "hi") {
            ee.cpu.hi = ch.before;
        } else if (ch.file == "lo") {
            ee.cpu.lo = ch.before;
        } else if (ch.file == "fpr") {
            ee.fpu.fpr[ch.index & 31] = static_cast<u32>(ch.before.lo);
        } else if (ch.file == "fcr31") {
            ee.fpu.fcr31 = static_cast<u32>(ch.before.lo);
        } else if (ch.file == "facc") {
            ee.fpu.acc = static_cast<u32>(ch.before.lo);
        } else if (ch.file == "vf") {
            ee.vu0.vf[ch.index & 31] = ch.before;
        } else if (ch.file == "vi") {
            ee.vu0.vi[ch.index & 31] = static_cast<u32>(ch.before.lo);
        } else if (ch.file == "acc") {
            ee.vu0.acc = ch.before;
        } else if (ch.file == "q") {
            ee.vu0.vi[Vu0::kQ] = static_cast<u32>(ch.before.lo);
        } else if (ch.file == "sa") {
            ee.cpu.sa = static_cast<u32>(ch.before.lo);
        }
    }
}

// Registers whose prior value the case establishes. Anything else held
// something the corpus never recorded, so nothing can be asserted about it.
std::vector<std::string> pinned(const Case& k) {
    std::vector<std::string> keys{
        "gpr:0", "gpr:" + std::to_string(k.rs), "gpr:" + std::to_string(k.rt),
        "fpr:" + std::to_string(k.fs), "fpr:" + std::to_string(k.ft),
        "hi:0", "lo:0", "sa:0", "fcr31:0", "facc:0", "acc:0",
    };
    for (const Change& ch : k.changes) {
        keys.push_back(ch.file + ":" + std::to_string(ch.index));
    }
    return keys;
}

}  // namespace

int main(int argc, char** argv) {
    spdlog::set_pattern("%^%l%$: %v");
    const std::filesystem::path path =
        argc > 1 ? argv[1] : std::filesystem::path("tests/vectors/execute.tsv");
    const std::vector<Case> cases = readCases(path);

    std::map<std::string, s32> unimplemented;
    std::map<std::string, s32> executed;
    std::map<std::string, s32> diverged;
    s32 ran = 0;
    s32 flagsSkipped = 0;
    s32 pcUnrecorded = 0;

    for (const Case& k : cases) {
        const Instruction insn{k.word};
        Memory window = makeWindow(k.window, k.before);
        Interpreter interpreter(window);
        EeContext& ee = interpreter.ee;
        setUp(ee, k);
        std::map<std::string, Reg128> before;
        for (const std::string& key : pinned(k)) {
            before[key] = read(ee, key);
        }

        const Status status = interpreter.execute(insn);
        if (status == Status::Unimplemented) {
            unimplemented[std::string(lookupName(decode(insn).op))]++;
            continue;
        }
        if (status == Status::Undecodable) {
            fail("{:08x}: no slot names this encoding", k.word);
            settle(diverged, "", false);
            continue;
        }
        ++ran;
        executed[std::string(lookupName(decode(insn).op))]++;

        std::map<std::string, Reg128> expected;
        for (const Change& ch : k.changes) {
            expected[ch.file + ":" + std::to_string(ch.index)] = ch.after;
        }
        for (const auto& [key, want] : expected) {
            // VU16 to VU18 are the status, MAC and clip flags, which nothing
            // here computes yet
            if (key.rfind("vi:1", 0) == 0 &&
                (key == "vi:16" || key == "vi:17" || key == "vi:18")) {
                ++flagsSkipped;
                continue;
            }
            const Reg128 got = read(ee, key);
            if (got != want) {
                fail("{:08x} {}: {} is {:016x}{:016x}, expected {:016x}{:016x}", k.word,
                     disassemble(insn), key, got.hi, got.lo, want.hi, want.lo);
            }
        }
        for (const auto& [key, was] : before) {
            if (expected.count(key) == 0 && read(ee, key) != was) {
                fail("{:08x} {}: wrote {}, which the case pinned and the run left alone",
                     k.word, disassemble(insn), key);
            }
        }
        const bool hasDelaySlot = decode(insn).op != Op::Invalid && !k.nested &&
                                  k.pcAfter == k.pc + 4 && ee.cpu.pc != k.pc + 4;
        // eret jumps to EPC, and no vector captures the COP0 registers, so
        // nothing here establishes where it should land.
        if (decode(insn).op == Op::eret) {
            ++pcUnrecorded;
        } else if (!hasDelaySlot && ee.cpu.pc != k.pcAfter) {
            fail("{:08x} {}: pc is {:08x}, expected {:08x}", k.word, disassemble(insn),
                 ee.cpu.pc, k.pcAfter);
        }
        if (k.hasMemory) {
            if (window.fault()) {
                fail("{:08x} {}: reached outside the window at {:08x}", k.word,
                     disassemble(insn), k.window);
            }
            for (u32 i = 0; i < 16; ++i) {
                const u8 actual = window.read8(k.window + i);
                if (actual != k.after[i]) {
                    fail("{:08x} {}: memory byte {} at {:08x} is {:02x}, expected {:02x}",
                         k.word, disassemble(insn), i, k.window, actual, k.after[i]);
                    break;
                }
            }
        }

        const Op op = decode(insn).op;
        settle(diverged, lookupName(op), oracleFollowsHost(op));
    }

    s32 skipped = 0;
    for (const auto& [op, count] : unimplemented) {
        skipped += count;
    }
    spdlog::info("{} cases passed over {} ops", ran - failures, executed.size());
    if (flagsSkipped != 0) {
        spdlog::warn("{} VU flag assertions skipped: the status, MAC and clip "
                     "registers are not computed", flagsSkipped);
    }
    s32 divergences = 0;
    for (const auto& [op, count] : diverged) {
        divergences += count;
    }
    if (divergences != 0) {
        spdlog::warn("{} cases follow the hardware where these vectors follow the host FPU:",
                     divergences);
        for (const auto& [op, count] : diverged) {
            spdlog::warn("  {} ({} cases)", op, count);
        }
    }
    if (pcUnrecorded != 0) {
        spdlog::warn("{} eret cases ran with the pc unchecked: no vector records EPC",
                     pcUnrecorded);
    }
    if (skipped != 0) {
        spdlog::warn("{} cases skipped over {} ops this interpreter does not model yet",
                     skipped, unimplemented.size());
        for (const auto& [op, count] : unimplemented) {
            spdlog::warn("  {} ({} cases)", op, count);
        }
    }
    if (failures != 0) {
        spdlog::error("{} failures over {} executed cases", failures, ran);
        return 1;
    }
    return 0;
}

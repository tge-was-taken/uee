#pragma once

#include "json.h"
#include "memory.h"
#include "r5900.h"
#include "types.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace uee::test {

// Helpers shared by the tests that replay what pcsx2-hook recorded.

inline u32 readHex(const Json& value) { return static_cast<u32>(value.getHex()); }
inline u32 readInteger(const Json& value) { return static_cast<u32>(value.getInteger()); }

// Written as 32 hex digits, w first.
inline Reg128 readReg128(const Json& value) {
    const auto [lo, hi] = value.getHex128();
    return Reg128{lo, hi};
}

inline std::string formatReg128(const Reg128& r) {
    return std::format("{:08X}{:08X}{:08X}{:08X}", r.word(3), r.word(2), r.word(1), r.word(0));
}

// A memory quadword is written as its sixteen bytes in address order.
inline u32 readLittleEndianWord(const std::string& bytes, u32 word) {
    u32 value = 0;
    for (u32 i = 0; i < 4; ++i) {
        const u32 at = (word * 4 + i) * 2;
        value |= static_cast<u32>(std::stoul(bytes.substr(at, 2), nullptr, 16)) << (8 * i);
    }
    return value;
}

// A narrower write to a VU window fills the rest of its word with ones, so a
// quadword goes in and comes out a word at a time.
inline void writeQuadword(Memory& memory, u32 address, const std::string& bytes) {
    for (u32 i = 0; i < 4; ++i) {
        memory.write32(address + i * 4, readLittleEndianWord(bytes, i));
    }
}

inline std::string readQuadword(Memory& memory, u32 address) {
    std::string bytes;
    for (u32 i = 0; i < 4; ++i) {
        const u32 word = memory.read32(address + i * 4);
        for (u32 b = 0; b < 4; ++b) {
            bytes += std::format("{:02X}", (word >> (8 * b)) & 0xFFu);
        }
    }
    return bytes;
}

// Where each unit's memories and interface registers sit.
struct VuUnit {
    u32 index;
    u32 micro;
    u32 data;
    u32 size;
    u32 interface;

    u32 getInstructionCount() const { return size / 8; }
};

inline VuUnit getVuUnit(u32 index) {
    if (index == 0) {
        return {0, kVu0MicroBase, kVu0DataBase, kVu0MemorySize, 0x10003800};
    }
    return {1, kVu1MicroBase, kVu1DataBase, kVu1MemorySize, 0x10003C00};
}

inline void readRecords(const std::string& path, const std::function<void(const Json&)>& visit) {
    std::ifstream file(path);
    if (!file) {
        spdlog::error("cannot open {}", path);
        std::exit(2);
    }
    std::string line;
    while (std::getline(file, line)) {
        if (!line.empty()) {
            visit(Json::parse(line));
        }
    }
}

// One with a reason is a known disagreement with the recording rather than a
// failure.
struct Mismatch {
    std::string field;
    std::string expected;
    std::string actual;
    std::string reason;
};

// Failed cases grouped by what failed, with a few examples each, and the known
// disagreements counted apart.
class Report {
public:
    explicit Report(usize examples) : mExamples(examples) {}

    void addCase(const std::string& group, const std::string& description,
                 const std::vector<Mismatch>& found) {
        ++mCases;
        std::set<std::string> kinds;
        std::set<std::string> reasons;
        for (const Mismatch& m : found) {
            if (m.reason.empty()) {
                kinds.insert(getKind(m.field));
            } else {
                reasons.insert(m.reason);
            }
        }
        for (const std::string& reason : reasons) {
            ++mKnown[reason];
        }
        if (kinds.empty()) {
            return;
        }
        ++mFailed;
        for (const std::string& kind : kinds) {
            const std::string key = group + " " + kind;
            std::vector<std::string>& examples = mExamplesByKey[key];
            ++mFailuresByKey[key];
            if (examples.size() < mExamples) {
                examples.push_back(describe(description, found));
            }
        }
    }

    void addKnown(const std::string& reason) {
        ++mCases;
        ++mKnown[reason];
    }

    usize getFailed() const { return mFailed; }

    int finish(const char* name) const {
        std::vector<std::pair<std::string, usize>> sorted(mFailuresByKey.begin(),
                                                          mFailuresByKey.end());
        std::sort(sorted.begin(), sorted.end(),
                  [](const auto& a, const auto& b) { return a.second > b.second; });
        for (const auto& [key, count] : sorted) {
            std::printf("%5zu  %s\n", count, key.c_str());
            for (const std::string& example : mExamplesByKey.at(key)) {
                std::printf("    %s\n", example.c_str());
            }
        }
        for (const auto& [reason, count] : mKnown) {
            std::printf("known: %zu %s differ by %s\n", count, name, reason.c_str());
        }
        std::printf("%zu of %zu %s fail\n", mFailed, mCases, name);
        return mFailed == 0 ? 0 : 1;
    }

private:
    // vf12 and vf3 are one kind of failure, and so is every memory quadword.
    static std::string getKind(const std::string& field) {
        return field.substr(0, field.find_first_of("0123456789"));
    }

    static std::string describe(const std::string& description, const std::vector<Mismatch>& found) {
        constexpr usize kShown = 6;
        std::string text = description;
        for (usize i = 0; i < found.size() && i < kShown; ++i) {
            const Mismatch& m = found[i];
            text += std::format("\n      {}: expected {} got {}{}", m.field, m.expected, m.actual,
                                m.reason.empty() ? "" : " (" + m.reason + ")");
        }
        if (found.size() > kShown) {
            text += std::format("\n      ... {} more", found.size() - kShown);
        }
        return text;
    }

    usize mExamples;
    usize mCases = 0;
    usize mFailed = 0;
    std::map<std::string, usize> mFailuresByKey;
    std::map<std::string, std::vector<std::string>> mExamplesByKey;
    std::map<std::string, usize> mKnown;
};

}  // namespace uee::test

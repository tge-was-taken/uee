#pragma once

#include "types.h"

#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace uee::test {

// Just enough JSON to read the vectors PCSX2 writes: one document per line,
// and no escapes beyond the ones a hex string or a mnemonic never needs.
class Json {
public:
    enum class Kind { Null, Bool, Number, String, Array, Object };

    static Json parse(std::string_view text) {
        Json value;
        usize at = 0;
        value.read(text, at);
        return value;
    }

    Kind getKind() const { return mKind; }
    bool isNull() const { return mKind == Kind::Null; }
    bool getBool() const {
        expect(Kind::Bool);
        return mText == "true";
    }
    const std::string& getText() const { return mText; }
    usize size() const { return mItems.size(); }

    const Json& operator[](usize index) const {
        expect(Kind::Array);
        if (index >= mItems.size()) {
            die("index " + std::to_string(index) + " past the end of an array of " +
                std::to_string(mItems.size()));
        }
        return mItems[index];
    }

    const Json* find(std::string_view key) const {
        expect(Kind::Object);
        for (usize i = 0; i < mKeys.size(); ++i) {
            if (mKeys[i] == key) {
                return &mItems[i];
            }
        }
        return nullptr;
    }

    const Json& operator[](std::string_view key) const {
        const Json* value = find(key);
        if (value == nullptr) {
            die("no field named " + std::string(key));
        }
        return *value;
    }

    const std::vector<Json>& getItems() const { return mItems; }

    u64 getInteger() const {
        expect(Kind::Number);
        return parseNumber(mText, 10);
    }

    s64 getSigned() const {
        expect(Kind::Number);
        s64 value = 0;
        std::from_chars(mText.data(), mText.data() + mText.size(), value);
        return value;
    }

    u64 getHex() const {
        expect(Kind::String);
        return parseNumber(mText, 16);
    }

    // A 128-bit value written as 32 hex digits, most significant first.
    std::pair<u64, u64> getHex128() const {
        expect(Kind::String);
        if (mText.size() != 32) {
            die("expected 32 hex digits, got \"" + mText + "\"");
        }
        return {parseNumber(mText.substr(16), 16), parseNumber(mText.substr(0, 16), 16)};
    }

private:
    [[noreturn]] static void die(const std::string& message) {
        std::fprintf(stderr, "json: %s\n", message.c_str());
        std::abort();
    }

    void expect(Kind kind) const {
        if (mKind != kind) {
            die("expected kind " + std::to_string(static_cast<int>(kind)) + ", found " +
                std::to_string(static_cast<int>(mKind)));
        }
    }

    static u64 parseNumber(std::string_view digits, int base) {
        u64 value = 0;
        const auto result = std::from_chars(digits.data(), digits.data() + digits.size(), value, base);
        if (result.ec != std::errc{} || result.ptr != digits.data() + digits.size()) {
            die("not a base " + std::to_string(base) + " number: \"" + std::string(digits) + "\"");
        }
        return value;
    }

    static void skipSpace(std::string_view text, usize& at) {
        while (at < text.size() && (text[at] == ' ' || text[at] == '\t' || text[at] == '\n' ||
                                    text[at] == '\r')) {
            ++at;
        }
    }

    static std::string readString(std::string_view text, usize& at) {
        std::string out;
        ++at;
        while (at < text.size() && text[at] != '"') {
            if (text[at] == '\\') {
                ++at;
            }
            out += text[at++];
        }
        ++at;
        return out;
    }

    void read(std::string_view text, usize& at) {
        skipSpace(text, at);
        if (at >= text.size()) {
            die("unexpected end of input");
        }
        const char c = text[at];
        if (c == '{') {
            mKind = Kind::Object;
            ++at;
            skipSpace(text, at);
            if (text[at] == '}') {
                ++at;
                return;
            }
            for (;;) {
                skipSpace(text, at);
                mKeys.push_back(readString(text, at));
                skipSpace(text, at);
                ++at;  // ':'
                mItems.emplace_back().read(text, at);
                skipSpace(text, at);
                if (text[at++] == '}') {
                    return;
                }
            }
        }
        if (c == '[') {
            mKind = Kind::Array;
            ++at;
            skipSpace(text, at);
            if (text[at] == ']') {
                ++at;
                return;
            }
            for (;;) {
                mItems.emplace_back().read(text, at);
                skipSpace(text, at);
                if (text[at++] == ']') {
                    return;
                }
            }
        }
        if (c == '"') {
            mKind = Kind::String;
            mText = readString(text, at);
            return;
        }
        const usize start = at;
        while (at < text.size() && text[at] != ',' && text[at] != '}' && text[at] != ']' &&
               text[at] != ' ') {
            ++at;
        }
        mText = std::string(text.substr(start, at - start));
        if (mText == "true" || mText == "false") {
            mKind = Kind::Bool;
        } else if (mText == "null") {
            mKind = Kind::Null;
        } else {
            mKind = Kind::Number;
        }
    }

    Kind mKind = Kind::Null;
    std::string mText;
    std::vector<std::string> mKeys;
    std::vector<Json> mItems;
};

}  // namespace uee::test

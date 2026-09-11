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
    static Json parse(std::string_view text) {
        Json value;
        usize at = 0;
        value.read(text, at);
        return value;
    }

    bool getBool() const {
        expect(Kind::Bool);
        return mText == "true";
    }
    const std::string& getText() const { return mText; }
    const std::vector<Json>& getItems() const { return mItems; }

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

    u64 getInteger() const {
        expect(Kind::Number);
        return parseNumber<u64>(mText, 10);
    }

    s64 getSigned() const {
        expect(Kind::Number);
        return parseNumber<s64>(mText, 10);
    }

    u64 getHex() const {
        expect(Kind::String);
        return parseNumber<u64>(mText, 16);
    }

    // A 128-bit value written as 32 hex digits, most significant first.
    std::pair<u64, u64> getHex128() const {
        expect(Kind::String);
        if (mText.size() != 32) {
            die("expected 32 hex digits, got \"" + mText + "\"");
        }
        return {parseNumber<u64>(mText.substr(16), 16), parseNumber<u64>(mText.substr(0, 16), 16)};
    }

private:
    enum class Kind { Null, Bool, Number, String, Array, Object };

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

    template <typename T>
    static T parseNumber(std::string_view digits, int base) {
        T value = 0;
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

    static char takeChar(std::string_view text, usize& at) {
        skipSpace(text, at);
        if (at >= text.size()) {
            die("unexpected end of input");
        }
        return text[at++];
    }

    static void takeExpected(std::string_view text, usize& at, char expected) {
        const char c = takeChar(text, at);
        if (c != expected) {
            die(std::string("expected '") + expected + "' at " + std::to_string(at - 1) +
                ", found '" + c + "'");
        }
    }

    static std::string readString(std::string_view text, usize& at) {
        takeExpected(text, at, '"');
        std::string out;
        while (at < text.size() && text[at] != '"') {
            if (text[at] == '\\') {
                ++at;
            }
            out += text[at++];
        }
        takeExpected(text, at, '"');
        return out;
    }

    bool takeEmpty(std::string_view text, usize& at, char close) {
        skipSpace(text, at);
        if (at < text.size() && text[at] == close) {
            ++at;
            return true;
        }
        return false;
    }

    bool takeSeparator(std::string_view text, usize& at, char close) {
        const char c = takeChar(text, at);
        if (c != ',' && c != close) {
            die(std::string("expected ',' or '") + close + "', found '" + c + "'");
        }
        return c == ',';
    }

    void readObject(std::string_view text, usize& at) {
        mKind = Kind::Object;
        if (takeEmpty(text, at, '}')) {
            return;
        }
        do {
            mKeys.push_back(readString(text, at));
            takeExpected(text, at, ':');
            mItems.emplace_back().read(text, at);
        } while (takeSeparator(text, at, '}'));
    }

    void readArray(std::string_view text, usize& at) {
        mKind = Kind::Array;
        if (takeEmpty(text, at, ']')) {
            return;
        }
        do {
            mItems.emplace_back().read(text, at);
        } while (takeSeparator(text, at, ']'));
    }

    void readScalar(std::string_view text, usize& at) {
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

    void read(std::string_view text, usize& at) {
        skipSpace(text, at);
        if (at >= text.size()) {
            die("unexpected end of input");
        }
        switch (text[at]) {
            case '{':
                ++at;
                readObject(text, at);
                return;
            case '[':
                ++at;
                readArray(text, at);
                return;
            case '"':
                mKind = Kind::String;
                mText = readString(text, at);
                return;
            default:
                readScalar(text, at);
        }
    }

    Kind mKind = Kind::Null;
    std::string mText;
    std::vector<std::string> mKeys;
    std::vector<Json> mItems;
};

}  // namespace uee::test

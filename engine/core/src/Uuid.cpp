#include "local3d/core/Uuid.hpp"

#include <array>
#include <cstdio>
#include <random>

namespace l3d {
namespace {

constexpr std::array<char, 16> kHexDigits = {'0', '1', '2', '3', '4', '5', '6', '7',
                                             '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};

[[nodiscard]] int HexValue(char c) noexcept {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

[[nodiscard]] u64 ParseHex(std::string_view text) noexcept {
    u64 value = 0;
    for (const char c : text) {
        const int digit = HexValue(c);
        if (digit < 0) {
            return 0;
        }
        value = (value << 4) | static_cast<u64>(digit);
    }
    return value;
}

} // namespace

Uuid Uuid::Generate() noexcept {
    // One thread local generator: random_device is slow and not thread safe
    // everywhere, and uuid generation happens on asset import threads.
    static thread_local std::mt19937_64 rng{std::random_device{}()};
    return Uuid{rng(), rng()};
}

bool Uuid::Parse(std::string_view text, Uuid& out) noexcept {
    // Expected layout: 8-4-4-4-12 (36 characters).
    if (text.size() != 36 || text[8] != '-' || text[13] != '-' || text[18] != '-' ||
        text[23] != '-') {
        return false;
    }
    // 128 bit big endian split: high = first 64 bits, low = last 64 bits.
    const u64 high = (ParseHex(text.substr(0, 8)) << 32) | (ParseHex(text.substr(9, 4)) << 16) |
                     ParseHex(text.substr(14, 4));
    const u64 low = (ParseHex(text.substr(19, 4)) << 48) | ParseHex(text.substr(24, 12));
    out = Uuid{high, low};
    return true;
}

usize Uuid::ToString(char* buffer, usize bufferSize) const noexcept {
    if (buffer == nullptr || bufferSize < 37) {
        return 0;
    }
    usize pos = 0;
    auto writeHex = [&buffer, &pos](u64 value, usize digits) {
        for (usize i = 0; i < digits; ++i) {
            const usize shift = (digits - 1 - i) * 4;
            buffer[pos++] = kHexDigits[(value >> shift) & 0xFULL];
        }
    };
    writeHex(high_ >> 32, 8);
    buffer[pos++] = '-';
    writeHex((high_ >> 16) & 0xFFFFULL, 4);
    buffer[pos++] = '-';
    writeHex(high_ & 0xFFFFULL, 4);
    buffer[pos++] = '-';
    writeHex(low_ >> 48, 4);
    buffer[pos++] = '-';
    writeHex(low_ & 0xFFFFFFFFFFFFULL, 12);
    buffer[pos] = '\0';
    return pos;
}

std::string Uuid::ToString() const {
    std::array<char, 37> buffer{};
    const usize length = ToString(buffer.data(), buffer.size());
    return std::string(buffer.data(), length);
}

std::string to_string(const Uuid& uuid) { return uuid.ToString(); }

} // namespace l3d

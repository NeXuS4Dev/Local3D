#pragma once
/// @file StaticString.hpp
/// @brief Fixed capacity, heap free string.  Used for names, paths and status
///        messages that must not allocate (log hot path, status objects, C ABI).

#include "local3d/core/Common.hpp"

#include "local3d/core/Format.hpp"

#include <array>
#include <string>
#include <string_view>

namespace l3d {

/// A null terminated string with compile time capacity `N` (including the
/// terminator).  Copy/move are trivial copies of the inline buffer.
template <usize N>
class StaticString {
    static_assert(N >= 1, "StaticString needs room for at least the terminator");

public:
    constexpr StaticString() noexcept = default;

    // NOLINTNEXTLINE(google-explicit-constructor)
    constexpr StaticString(std::string_view text) noexcept { Assign(text); }

    /// Construct from a printf-style format.  Deliberately a named function
    /// rather than a constructor so truncation is visible at the call site.
    template <typename... Args>
    [[nodiscard]] static StaticString Format(std::string_view format, Args&&... args);

    constexpr void Assign(std::string_view text) noexcept {
        length_ = text.size() < Capacity ? text.size() : Capacity;
        for (usize i = 0; i < length_; ++i) {
            data_[i] = text[i];
        }
        data_[length_] = '\0';
    }

    constexpr void Append(std::string_view text) noexcept {
        for (const char c : text) {
            if (length_ >= Capacity) {
                break;
            }
            data_[length_++] = c;
        }
        data_[length_] = '\0';
    }

    constexpr void Clear() noexcept {
        length_ = 0;
        data_[0] = '\0';
    }

    [[nodiscard]] constexpr std::string_view View() const noexcept { return {data_.data(), length_}; }
    [[nodiscard]] constexpr const char* CStr() const noexcept { return data_.data(); }
    [[nodiscard]] constexpr usize Size() const noexcept { return length_; }
    [[nodiscard]] constexpr bool Empty() const noexcept { return length_ == 0; }
    [[nodiscard]] constexpr usize CapacityLeft() const noexcept { return Capacity - length_; }
    /// True when an Assign/Append had to drop characters.
    [[nodiscard]] constexpr bool Truncated() const noexcept { return truncated_; }

    [[nodiscard]] constexpr char& operator[](usize index) noexcept { return data_[index]; }
    [[nodiscard]] constexpr char operator[](usize index) const noexcept { return data_[index]; }

    // NOLINTNEXTLINE(google-explicit-constructor)
    constexpr operator std::string_view() const noexcept { return View(); }

    [[nodiscard]] std::string ToStdString() const { return std::string(View()); }

    friend constexpr bool operator==(const StaticString& a, const StaticString& b) noexcept {
        return a.View() == b.View();
    }
    friend constexpr bool operator!=(const StaticString& a, const StaticString& b) noexcept {
        return !(a == b);
    }
    friend constexpr bool operator==(const StaticString& a, std::string_view b) noexcept {
        return a.View() == b;
    }

private:
    static constexpr usize Capacity = N - 1;
    std::array<char, N> data_{};
    usize length_ = 0;
    bool truncated_ = false;
};

template <usize N>
template <typename... Args>
StaticString<N> StaticString<N>::Format(std::string_view format, Args&&... args) {
    std::string scratch;
    scratch.reserve(N);
    fmt::FormatTo(scratch, format, args...);
    StaticString<N> result;
    result.Assign(scratch);
    result.truncated_ = scratch.size() > Capacity;
    return result;
}

/// Common aliases.
using NameString = StaticString<64>;
using PathString = StaticString<256>;

} // namespace l3d

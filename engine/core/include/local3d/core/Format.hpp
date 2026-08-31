#pragma once
/// @file Format.hpp
/// @brief A tiny `{}`-style string formatter.
///
/// Why not std::format?  It is not available on the oldest compilers Local3D
/// supports, and the engine only needs positional substitution plus a fixed
/// precision specifier.  This implementation allocates only into the caller
/// supplied std::string, never throws, and is fast enough for logging.
///
/// Supported syntax:
///   "{}"       - next argument
///   "{2}"      - explicit argument index
///   "{{" "}}"  - literal braces
///   "{:.3f}"   - fixed point with 3 decimals (floating point only)
///   "{:x}"     - hexadecimal (integral only)

#include "local3d/core/Common.hpp"

#include <charconv>
#include <cstdio>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>

namespace l3d::fmt {

/// Append a single value.  Extend via ADL: define `to_string(const T&)` next to
/// your type and it will be picked up automatically.
template <typename T>
void WriteValue(std::string& out, const T& value, std::string_view spec) {
    L3D_UNUSED(spec); // Not every type has a format specifier.
    if constexpr (std::is_same_v<T, bool>) {
        out.append(value ? "true" : "false");
    } else if constexpr (std::is_same_v<T, char>) {
        out.push_back(value);
    } else if constexpr (std::is_floating_point_v<T>) {
        // Parse an optional ".Nf" precision specifier.
        int precision = -1;
        if (spec.size() >= 3 && spec[0] == ':' && spec[1] == '.') {
            precision = 0;
            for (usize i = 2; i < spec.size(); ++i) {
                if (spec[i] >= '0' && spec[i] <= '9') {
                    precision = precision * 10 + (spec[i] - '0');
                } else {
                    break;
                }
            }
        }
        char buffer[64]{};
        int written = 0;
        if (precision >= 0) {
            written = std::snprintf(buffer, sizeof(buffer), "%.*f", precision,
                                    static_cast<double>(value));
        } else {
            const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value);
            written = static_cast<int>(result.ptr - buffer);
        }
        out.append(buffer, static_cast<usize>(written > 0 ? written : 0));
    } else if constexpr (std::is_integral_v<T>) {
        char buffer[32]{};
        const bool hex = !spec.empty() && spec.back() == 'x';
        const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value,
                                          hex ? 16 : 10);
        out.append(buffer, static_cast<usize>(result.ptr - buffer));
    } else if constexpr (std::is_pointer_v<T>) {
        char buffer[32]{};
        const int written =
            std::snprintf(buffer, sizeof(buffer), "%p", static_cast<const void*>(value));
        out.append(buffer, static_cast<usize>(written > 0 ? written : 0));
    } else if constexpr (std::is_enum_v<T>) {
        WriteValue(out, static_cast<std::underlying_type_t<T>>(value), spec);
    } else if constexpr (requires { to_string(value); }) {
        out.append(to_string(value));
    } else if constexpr (requires { value.ToString(); }) {
        out.append(value.ToString());
    } else {
        out.append("<unprintable>");
    }
}

/// Overloads for the string-ish types (declared before the generic template is
/// instantiated, so they win during overload resolution).
inline void WriteValue(std::string& out, std::string_view value, std::string_view /*spec*/) {
    out.append(value);
}
inline void WriteValue(std::string& out, const std::string& value, std::string_view /*spec*/) {
    out.append(value);
}
inline void WriteValue(std::string& out, const char* value, std::string_view /*spec*/) {
    out.append(value != nullptr ? value : "(null)");
}

namespace detail {

template <std::size_t I, typename Tuple>
void WriteArg(std::string& out, const Tuple& args, std::string_view spec) {
    WriteValue(out, std::get<I>(args), spec);
}

template <typename Tuple, std::size_t... Is>
void WriteArgByIndex(usize index, std::string& out, const Tuple& args, std::string_view spec,
                     std::index_sequence<Is...> /*indices*/) {
    L3D_UNUSED(spec); // Only reachable through the fold expression below.
    const bool written = ((Is == index ? (WriteArg<Is>(out, args, spec), true) : false) || ...);
    if (!written) {
        out.append("<bad-arg-index>");
    }
}

/// Walk the format string, appending literals and substituted arguments.
template <typename Tuple>
void FormatInto(std::string& out, std::string_view format, const Tuple& args, usize& nextArg) {
    constexpr usize kArgCount = std::tuple_size_v<Tuple>;
    usize i = 0;
    while (i < format.size()) {
        const char c = format[i];
        if (c != '{' && c != '}') {
            out.push_back(c);
            ++i;
            continue;
        }
        if (i + 1 < format.size() && format[i + 1] == c) { // escaped brace
            out.push_back(c);
            i += 2;
            continue;
        }
        if (c == '}') { // stray closing brace
            out.push_back(c);
            ++i;
            continue;
        }
        // Parse "{[index][:spec]}"
        usize close = format.find('}', i);
        if (close == std::string_view::npos) {
            out.push_back(c);
            ++i;
            continue;
        }
        const std::string_view body = format.substr(i + 1, close - i - 1);
        usize argIndex = nextArg;
        usize cursor = 0;
        if (!body.empty() && body[0] >= '0' && body[0] <= '9') {
            argIndex = 0;
            while (cursor < body.size() && body[cursor] >= '0' && body[cursor] <= '9') {
                argIndex = argIndex * 10 + static_cast<usize>(body[cursor] - '0');
                ++cursor;
            }
        } else {
            ++nextArg;
        }
        const std::string_view spec = cursor < body.size() ? body.substr(cursor) : std::string_view{};
        if (argIndex < kArgCount) {
            WriteArgByIndex(argIndex, out, args, spec, std::make_index_sequence<kArgCount>{});
        } else {
            out.append("<missing-arg>");
        }
        i = close + 1;
    }
}

} // namespace detail

/// Append the formatted result to `out`.
template <typename... Args>
void FormatTo(std::string& out, std::string_view format, const Args&... args) {
    const std::tuple<const Args&...> argTuple(args...);
    usize nextArg = 0;
    detail::FormatInto(out, format, argTuple, nextArg);
}

/// Format into a new string.
template <typename... Args>
[[nodiscard]] std::string Format(std::string_view format, const Args&... args) {
    std::string out;
    FormatTo(out, format, args...);
    return out;
}

/// Format into a fixed stack buffer (used by the logging fast path so that a
/// log call does not allocate when the string fits).  Returns the number of
/// characters written, excluding the null terminator.
template <usize N, typename... Args>
usize FormatToBuffer(char (&buffer)[N], std::string_view format, const Args&... args) {
    std::string scratch;
    scratch.reserve(N);
    FormatTo(scratch, format, args...);
    const usize count = scratch.size() < N ? scratch.size() : N - 1;
    for (usize i = 0; i < count; ++i) {
        buffer[i] = scratch[i];
    }
    buffer[count] = '\0';
    return count;
}

} // namespace l3d::fmt

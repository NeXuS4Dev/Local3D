#pragma once
/// @file Status.hpp
/// @brief Small, allocation free error type used across module boundaries.

#include "local3d/core/Common.hpp"
#include "local3d/core/Enum.hpp"

#include <array>
#include <string_view>

namespace l3d {

/// Categories of failure.  Deliberately small: subsystems add *context* through
/// the message, not through an explosion of codes.
enum class StatusCode : u16 {
    Ok = 0,
    InvalidArgument,
    InvalidState,
    NotFound,
    AlreadyExists,
    OutOfRange,
    OutOfMemory,
    IoError,
    ParseError,
    Unsupported,
    Timeout,
    NotInitialized,
    DeviceLost,
    Cancelled,
    Internal,
};

/// Maximum inline message length.  Messages longer than this are truncated -
/// status objects live on the stack in hot paths and must not allocate.
inline constexpr usize kStatusMessageCapacity = 96;

/// An error code plus a short, human readable message.  Trivially copyable,
/// allocation free, safe to return by value from any function.
class Status {
public:
    Status() noexcept = default;

    explicit Status(StatusCode code) noexcept : code_(code) {}

    Status(StatusCode code, std::string_view message) noexcept : code_(code) {
        SetMessage(message);
    }

    [[nodiscard]] static Status Ok() noexcept { return Status{}; }

    [[nodiscard]] constexpr bool IsOk() const noexcept { return code_ == StatusCode::Ok; }
    [[nodiscard]] constexpr bool IsError() const noexcept { return code_ != StatusCode::Ok; }
    [[nodiscard]] constexpr explicit operator bool() const noexcept { return IsOk(); }

    [[nodiscard]] constexpr StatusCode Code() const noexcept { return code_; }

    [[nodiscard]] constexpr std::string_view Message() const noexcept {
        return {message_.data(), messageLength_};
    }

    void SetMessage(std::string_view message) noexcept {
        const usize count = message.size() < kStatusMessageCapacity
                                ? message.size()
                                : kStatusMessageCapacity;
        for (usize i = 0; i < count; ++i) {
            message_[i] = message[i];
        }
        messageLength_ = static_cast<u8>(count);
    }

    /// "InvalidArgument: some context" - handy for logging.
    [[nodiscard]] std::string ToString() const;

    friend constexpr bool operator==(const Status& a, const Status& b) noexcept {
        return a.code_ == b.code_;
    }

private:
    StatusCode code_ = StatusCode::Ok;
    u8 messageLength_ = 0;
    std::array<char, kStatusMessageCapacity> message_{};
};

/// Human readable name of a status code (for logs and the editor console).
[[nodiscard]] std::string_view StatusCodeName(StatusCode code) noexcept;

} // namespace l3d

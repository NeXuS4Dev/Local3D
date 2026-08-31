#pragma once
/// @file BinaryStream.hpp
/// @brief Little endian binary reader/writer used by cooked assets and network
///        packets.
///
/// Format guarantees (see docs/architecture/serialization.md):
///  * fixed width integers, little endian, regardless of host;
///  * varint encoding for 32/64 bit unsigned values;
///  * strings are varint length + bytes, no null terminator;
///  * floats are IEEE 754 single/double.

#include "local3d/core/Common.hpp"
#include "local3d/core/Result.hpp"

#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace l3d::serial {

/// Appends to a growable byte buffer.  Never throws; callers check Status.
class BinaryWriter {
public:
    void WriteU8(u8 value);
    void WriteI32(i32 value);
    void WriteU32(u32 value);
    void WriteI64(i64 value);
    void WriteU64(u64 value);
    void WriteF32(f32 value);
    void WriteF64(f64 value);
    void WriteVarUint(u64 value);
    void WriteString(std::string_view text);
    void WriteBytes(ConstByteSpan bytes);
    /// Raw struct dump.  Only valid for types with stable layout that the
    /// engine itself defines and versions.
    void WriteRaw(ConstByteSpan bytes) { WriteBytes(bytes); }

    [[nodiscard]] const std::vector<u8>& Bytes() const noexcept { return buffer_; }
    [[nodiscard]] std::vector<u8> TakeBytes() noexcept { return std::move(buffer_); }
    [[nodiscard]] usize Size() const noexcept { return buffer_.size(); }

private:
    std::vector<u8> buffer_;
};

/// Reads from an immutable byte range.  Every read is bounds checked; hitting
/// the end sets an error state and returns zero values rather than reading
/// out of bounds.
class BinaryReader {
public:
    explicit BinaryReader(ConstByteSpan data) noexcept : data_(data) {}

    [[nodiscard]] u8 ReadU8();
    [[nodiscard]] i32 ReadI32();
    [[nodiscard]] u32 ReadU32();
    [[nodiscard]] i64 ReadI64();
    [[nodiscard]] u64 ReadU64();
    [[nodiscard]] f32 ReadF32();
    [[nodiscard]] f64 ReadF64();
    [[nodiscard]] u64 ReadVarUint();
    [[nodiscard]] std::string ReadString();
    /// Returns an empty span and sets the error state on failure.
    [[nodiscard]] ConstByteSpan ReadBytes(usize count);
    template <typename T>
    [[nodiscard]] T ReadRaw() {
        const ConstByteSpan bytes = ReadBytes(sizeof(T));
        T value{};
        if (bytes.size() == sizeof(T)) {
            std::memcpy(&value, bytes.data(), sizeof(T));
        }
        return value;
    }

    [[nodiscard]] bool HasError() const noexcept { return error_; }
    [[nodiscard]] usize Position() const noexcept { return position_; }
    [[nodiscard]] usize Remaining() const noexcept {
        return position_ < data_.size() ? data_.size() - position_ : 0;
    }
    void Skip(usize count);

private:
    bool EnsureAvailable(usize count) noexcept;

    ConstByteSpan data_;
    usize position_ = 0;
    bool error_ = false;
};

/// Read/validate a "MAGIC" + version header.  Returns the version on success.
[[nodiscard]] Result<u32> ReadVersionedHeader(BinaryReader& reader, u32 expectedMagic,
                                              u32 maxSupportedVersion);
void WriteVersionedHeader(BinaryWriter& writer, u32 magic, u32 version);

} // namespace l3d::serial

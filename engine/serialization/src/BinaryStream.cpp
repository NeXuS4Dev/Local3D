#include "local3d/serialization/BinaryStream.hpp"

#include "local3d/core/Assert.hpp"

#include <cstring>

namespace l3d::serial {
namespace {

void AppendLittleEndian(std::vector<u8>& buffer, const void* data, usize size) {
    const auto* bytes = static_cast<const u8*>(data);
    // Local3D only supports little endian hosts today; the explicit byte order
    // here keeps cooked assets portable if that ever changes.
#if defined(__BYTE_ORDER__) && (__BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)
    for (usize i = 0; i < size; ++i) {
        buffer.push_back(bytes[size - 1 - i]);
    }
#else
    buffer.insert(buffer.end(), bytes, bytes + size);
#endif
}

} // namespace

void BinaryWriter::WriteU8(u8 value) { buffer_.push_back(value); }

void BinaryWriter::WriteI32(i32 value) { AppendLittleEndian(buffer_, &value, sizeof(value)); }
void BinaryWriter::WriteU32(u32 value) { AppendLittleEndian(buffer_, &value, sizeof(value)); }
void BinaryWriter::WriteI64(i64 value) { AppendLittleEndian(buffer_, &value, sizeof(value)); }
void BinaryWriter::WriteU64(u64 value) { AppendLittleEndian(buffer_, &value, sizeof(value)); }
void BinaryWriter::WriteF32(f32 value) { AppendLittleEndian(buffer_, &value, sizeof(value)); }
void BinaryWriter::WriteF64(f64 value) { AppendLittleEndian(buffer_, &value, sizeof(value)); }

void BinaryWriter::WriteVarUint(u64 value) {
    while (value >= 0x80) {
        buffer_.push_back(static_cast<u8>((value & 0x7F) | 0x80));
        value >>= 7;
    }
    buffer_.push_back(static_cast<u8>(value));
}

void BinaryWriter::WriteString(std::string_view text) {
    WriteVarUint(text.size());
    buffer_.insert(buffer_.end(), text.begin(), text.end());
}

void BinaryWriter::WriteBytes(ConstByteSpan bytes) {
    for (const std::byte byte : bytes) {
        buffer_.push_back(static_cast<u8>(byte));
    }
}

bool BinaryReader::EnsureAvailable(usize count) noexcept {
    if (position_ + count > data_.size()) {
        error_ = true;
        position_ = data_.size();
        return false;
    }
    return true;
}

u8 BinaryReader::ReadU8() {
    if (!EnsureAvailable(1)) {
        return 0;
    }
    return static_cast<u8>(data_[position_++]);
}

namespace {
template <typename T>
T ReadLittleEndian(const u8* source) {
    T value{};
#if defined(__BYTE_ORDER__) && (__BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)
    auto* bytes = reinterpret_cast<u8*>(&value);
    for (usize i = 0; i < sizeof(T); ++i) {
        bytes[i] = source[sizeof(T) - 1 - i];
    }
#else
    std::memcpy(&value, source, sizeof(T));
#endif
    return value;
}
} // namespace

i32 BinaryReader::ReadI32() {
    if (!EnsureAvailable(sizeof(i32))) {
        return 0;
    }
    const i32 value = ReadLittleEndian<i32>(reinterpret_cast<const u8*>(data_.data()) + position_);
    position_ += sizeof(i32);
    return value;
}

u32 BinaryReader::ReadU32() {
    if (!EnsureAvailable(sizeof(u32))) {
        return 0;
    }
    const u32 value = ReadLittleEndian<u32>(reinterpret_cast<const u8*>(data_.data()) + position_);
    position_ += sizeof(u32);
    return value;
}

i64 BinaryReader::ReadI64() {
    if (!EnsureAvailable(sizeof(i64))) {
        return 0;
    }
    const i64 value = ReadLittleEndian<i64>(reinterpret_cast<const u8*>(data_.data()) + position_);
    position_ += sizeof(i64);
    return value;
}

u64 BinaryReader::ReadU64() {
    if (!EnsureAvailable(sizeof(u64))) {
        return 0;
    }
    const u64 value = ReadLittleEndian<u64>(reinterpret_cast<const u8*>(data_.data()) + position_);
    position_ += sizeof(u64);
    return value;
}

f32 BinaryReader::ReadF32() {
    if (!EnsureAvailable(sizeof(f32))) {
        return 0.0f;
    }
    const f32 value = ReadLittleEndian<f32>(reinterpret_cast<const u8*>(data_.data()) + position_);
    position_ += sizeof(f32);
    return value;
}

f64 BinaryReader::ReadF64() {
    if (!EnsureAvailable(sizeof(f64))) {
        return 0.0;
    }
    const f64 value = ReadLittleEndian<f64>(reinterpret_cast<const u8*>(data_.data()) + position_);
    position_ += sizeof(f64);
    return value;
}

u64 BinaryReader::ReadVarUint() {
    u64 value = 0;
    u32 shift = 0;
    for (u32 i = 0; i < 10; ++i) {
        const u8 byte = ReadU8();
        if (error_) {
            return 0;
        }
        value |= static_cast<u64>(byte & 0x7F) << shift;
        if ((byte & 0x80) == 0) {
            return value;
        }
        shift += 7;
    }
    error_ = true; // Varint longer than 64 bits: corrupt data.
    return 0;
}

std::string BinaryReader::ReadString() {
    const u64 length = ReadVarUint();
    if (error_ || length > Remaining()) {
        error_ = true;
        return {};
    }
    const std::string text(reinterpret_cast<const char*>(reinterpret_cast<const u8*>(data_.data()) + position_),
                           static_cast<usize>(length));
    position_ += static_cast<usize>(length);
    return text;
}

ConstByteSpan BinaryReader::ReadBytes(usize count) {
    if (!EnsureAvailable(count)) {
        return {};
    }
    const ConstByteSpan span = data_.subspan(position_, count);
    position_ += count;
    return span;
}

void BinaryReader::Skip(usize count) {
    if (!EnsureAvailable(count)) {
        return;
    }
    position_ += count;
}

void WriteVersionedHeader(BinaryWriter& writer, u32 magic, u32 version) {
    writer.WriteU32(magic);
    writer.WriteU32(version);
}

Result<u32> ReadVersionedHeader(BinaryReader& reader, u32 expectedMagic, u32 maxSupportedVersion) {
    const u32 magic = reader.ReadU32();
    const u32 version = reader.ReadU32();
    if (reader.HasError()) {
        return Unexpected(Status{StatusCode::IoError, "Truncated header"});
    }
    if (magic != expectedMagic) {
        return Unexpected(Status{StatusCode::ParseError, "Unexpected file magic"});
    }
    if (version == 0 || version > maxSupportedVersion) {
        return Unexpected(Status{StatusCode::Unsupported, "Unsupported file version"});
    }
    return version;
}

} // namespace l3d::serial

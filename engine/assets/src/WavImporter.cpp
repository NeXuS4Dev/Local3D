/// @file WavImporter.cpp
/// @brief Imports RIFF/WAVE audio into AudioData.
///
/// WAV is a RIFF container, which is a dozen lines to walk, so it is parsed here
/// rather than through a dependency.  Every supported source encoding is
/// normalised to 32 bit float interleaved frames - one sample type for the whole
/// audio system, at the cost of memory that streaming keeps bounded.
///
/// Supported: PCM at 8/16/24/32 bits, IEEE float at 32/64 bits, and the
/// WAVE_FORMAT_EXTENSIBLE wrapper around both.  Compressed WAVE formats (ADPCM,
/// GSM, MP3-in-WAV) are reported as unsupported instead of decoding to noise.

#include "local3d/assets/Importer.hpp"

#include <cstring>

namespace l3d::assets {

namespace {

constexpr u16 kFormatPcm = 1;
constexpr u16 kFormatFloat = 3;
constexpr u16 kFormatExtensible = 0xFFFE;

[[nodiscard]] bool TagIs(ConstByteSpan bytes, usize offset, const char (&tag)[5]) noexcept {
    if (offset + 4 > bytes.size()) {
        return false;
    }
    return std::memcmp(bytes.data() + offset, tag, 4) == 0;
}

[[nodiscard]] u32 ReadU32(ConstByteSpan bytes, usize offset) noexcept {
    u32 value = 0;
    std::memcpy(&value, bytes.data() + offset, sizeof(value));
    return value;
}

[[nodiscard]] u16 ReadU16(ConstByteSpan bytes, usize offset) noexcept {
    u16 value = 0;
    std::memcpy(&value, bytes.data() + offset, sizeof(value));
    return value;
}

struct WavFormat {
    u16 formatTag = 0;
    u16 channels = 0;
    u32 sampleRate = 0;
    u16 bitsPerSample = 0;
};

class WavImporter final : public IImporter {
public:
    [[nodiscard]] std::string_view Name() const noexcept override { return "wav"; }
    [[nodiscard]] u32 Version() const noexcept override { return 1; }
    [[nodiscard]] AssetType OutputType() const noexcept override { return AssetType::AudioClip; }

    [[nodiscard]] bool CanImport(const AssetPath& path) const noexcept override {
        return path.Extension() == ".wav";
    }

    [[nodiscard]] Result<ImportedAsset> Import(ConstByteSpan sourceBytes,
                                              const serial::JsonValue& settings,
                                              const AssetPath& sourcePath,
                                              ImportLog& log) override {
        L3D_UNUSED(settings);
        if (sourceBytes.size() < 12 || !TagIs(sourceBytes, 0, "RIFF") ||
            !TagIs(sourceBytes, 8, "WAVE")) {
            return Unexpected(Status{StatusCode::ParseError, "Not a RIFF/WAVE file"});
        }

        ConstByteSpan fmtChunk;
        ConstByteSpan dataChunk;
        if (auto chunks = WalkChunks(sourceBytes, fmtChunk, dataChunk); chunks.IsError()) {
            return Unexpected(chunks.Error());
        }
        if (fmtChunk.size() < 16) {
            return Unexpected(Status{StatusCode::ParseError, "WAVE file has no usable fmt chunk"});
        }
        if (dataChunk.empty()) {
            return Unexpected(Status{StatusCode::ParseError, "WAVE file has no data chunk"});
        }

        WavFormat format;
        format.formatTag = ReadU16(fmtChunk, 0);
        format.channels = ReadU16(fmtChunk, 2);
        format.sampleRate = ReadU32(fmtChunk, 4);
        format.bitsPerSample = ReadU16(fmtChunk, 14);

        if (format.formatTag == kFormatExtensible) {
            if (fmtChunk.size() < 26) {
                return Unexpected(
                    Status{StatusCode::ParseError, "WAVE_FORMAT_EXTENSIBLE fmt chunk is truncated"});
            }
            // The real format code is the first field of the sub-format GUID.
            format.formatTag = ReadU16(fmtChunk, 24);
            format.bitsPerSample = ReadU16(fmtChunk, 18); // Valid bits per sample.
            log.Warning("WAVE_FORMAT_EXTENSIBLE decoded using its sub-format tag");
        }

        if (format.channels == 0 || format.sampleRate == 0) {
            return Unexpected(Status{StatusCode::ParseError, "WAVE header has no channels or rate"});
        }
        if (format.formatTag != kFormatPcm && format.formatTag != kFormatFloat) {
            return Unexpected(Status{StatusCode::Unsupported,
                                     "Only PCM and IEEE float WAVE files are supported"});
        }
        if (format.formatTag == kFormatFloat && format.bitsPerSample != 32 &&
            format.bitsPerSample != 64) {
            return Unexpected(
                Status{StatusCode::Unsupported, "Float WAVE files must be 32 or 64 bit"});
        }
        if (format.formatTag == kFormatPcm && format.bitsPerSample != 8 &&
            format.bitsPerSample != 16 && format.bitsPerSample != 24 &&
            format.bitsPerSample != 32) {
            return Unexpected(
                Status{StatusCode::Unsupported, "PCM WAVE files must be 8/16/24/32 bit"});
        }

        AudioData audio;
        audio.name = std::string(sourcePath.Stem());
        audio.channels = format.channels;
        audio.sampleRate = format.sampleRate;

        const usize bytesPerSample = format.bitsPerSample / 8;
        const usize frameBytes = static_cast<usize>(bytesPerSample) * format.channels;
        if (frameBytes == 0) {
            return Unexpected(Status{StatusCode::ParseError, "WAVE frame size is zero"});
        }
        const usize frameCount = dataChunk.size() / frameBytes;
        if (frameCount * frameBytes != dataChunk.size()) {
            log.Warning("WAVE data chunk has a trailing partial frame which was dropped");
        }

        audio.frameCount = static_cast<u32>(frameCount);
        audio.samples.resize(frameCount * format.channels);
        if (auto converted = ConvertSamples(dataChunk, format, audio.samples);
            converted.IsError()) {
            return Unexpected(converted.Error());
        }
        if (!audio.IsValid()) {
            return Unexpected(Status{StatusCode::Internal, "Imported audio failed validation"});
        }

        AudioDocument document;
        document.audio = std::move(audio);
        return ImportedAsset{std::move(document)};
    }

private:
    [[nodiscard]] static Result<void> WalkChunks(ConstByteSpan bytes, ConstByteSpan& fmtChunk,
                                                 ConstByteSpan& dataChunk) {
        const u32 riffSize = ReadU32(bytes, 4);
        const u64 limit = static_cast<u64>(riffSize) + 8 < bytes.size()
                              ? static_cast<u64>(riffSize) + 8
                              : bytes.size();
        u64 offset = 12;
        while (offset + 8 <= limit) {
            const u32 size = ReadU32(bytes, static_cast<usize>(offset) + 4);
            const u64 payload = offset + 8;
            if (payload + size > bytes.size()) {
                return Unexpected(Status{StatusCode::ParseError, "WAVE chunk runs past end of file"});
            }
            const ConstByteSpan chunk(bytes.data() + payload, size);
            if (TagIs(bytes, static_cast<usize>(offset), "fmt ") && fmtChunk.empty()) {
                fmtChunk = chunk;
            } else if (TagIs(bytes, static_cast<usize>(offset), "data") && dataChunk.empty()) {
                dataChunk = chunk;
            }
            // Chunks are word aligned: an odd length carries one pad byte.
            offset = payload + size + (size % 2);
        }
        return {};
    }

    [[nodiscard]] static Result<void> ConvertSamples(ConstByteSpan data, const WavFormat& format,
                                                     std::vector<f32>& out) {
        const usize sampleCount = out.size();
        const usize bytesPerSample = format.bitsPerSample / 8;
        for (usize i = 0; i < sampleCount; ++i) {
            const usize at = i * bytesPerSample;
            if (at + bytesPerSample > data.size()) {
                return Unexpected(Status{StatusCode::ParseError, "WAVE data chunk is too short"});
            }
            out[i] = SampleToFloat(data, at, format);
        }
        return {};
    }

    [[nodiscard]] static f32 SampleToFloat(ConstByteSpan data, usize at,
                                           const WavFormat& format) noexcept {
        if (format.formatTag == kFormatFloat) {
            if (format.bitsPerSample == 32) {
                f32 value = 0.0f;
                std::memcpy(&value, data.data() + at, sizeof(value));
                return value;
            }
            f64 value = 0.0;
            std::memcpy(&value, data.data() + at, sizeof(value));
            return static_cast<f32>(value);
        }
        switch (format.bitsPerSample) {
            case 8: {
                // 8 bit PCM is unsigned with 128 as silence.
                return (static_cast<f32>(data[at]) - 128.0f) / 128.0f;
            }
            case 16: {
                i16 raw = 0;
                std::memcpy(&raw, data.data() + at, sizeof(raw));
                return static_cast<f32>(raw) / 32768.0f;
            }
            case 24: {
                // Little endian 24 bit, sign extended by hand.
                u32 raw = static_cast<u32>(data[at]) |
                          (static_cast<u32>(data[at + 1]) << 8) |
                          (static_cast<u32>(data[at + 2]) << 16);
                if ((raw & 0x800000U) != 0) {
                    raw |= 0xFF000000U;
                }
                return static_cast<f32>(static_cast<i32>(raw)) / 8388608.0f;
            }
            case 32: {
                i32 raw = 0;
                std::memcpy(&raw, data.data() + at, sizeof(raw));
                return static_cast<f32>(raw) / 2147483648.0f;
            }
            default: return 0.0f;
        }
    }
};

} // namespace

std::unique_ptr<IImporter> CreateWavImporter() {
    return std::make_unique<WavImporter>();
}

} // namespace l3d::assets

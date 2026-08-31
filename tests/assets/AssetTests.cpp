// Asset pipeline tests.
//
// These run entirely against MemoryFileSystem, so they are fast and leave no
// temporary directories behind, and they cover the whole chain: identity,
// importers, cooking, the cooked formats, the runtime loader and the upload into
// the null RHI plus the renderer.
#include "doctest.h"

#include "local3d/assets/AssetData.hpp"
#include "local3d/assets/AssetDatabase.hpp"
#include "local3d/assets/AssetId.hpp"
#include "local3d/assets/AssetManager.hpp"
#include "local3d/assets/AssetMeta.hpp"
#include "local3d/assets/Cooker.hpp"
#include "local3d/assets/FileSystem.hpp"
#include "local3d/assets/GpuUpload.hpp"
#include "local3d/assets/Importer.hpp"
#include "local3d/renderer/Renderer.hpp"

#include "stb_image_write.h"

#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

using namespace l3d;
using namespace l3d::assets;

namespace {

[[nodiscard]] ConstByteSpan Bytes(const std::vector<u8>& data) {
    return std::as_bytes(std::span(data.data(), data.size()));
}

/// Appends bytes for stb's writer callbacks.
void AppendCallback(void* context, void* data, int size) {
    auto* out = static_cast<std::vector<u8>*>(context);
    const auto* first = static_cast<const u8*>(data);
    out->insert(out->end(), first, first + static_cast<usize>(size));
}

// --- Binary builders -------------------------------------------------------

class BinWriter {
public:
    [[nodiscard]] u32 Offset() const noexcept { return static_cast<u32>(bytes.size()); }
    [[nodiscard]] const std::vector<u8>& Bytes() const noexcept { return bytes; }

    void F32(f32 value) { Append(&value, sizeof(value)); }
    void U8(u8 value) { Append(&value, sizeof(value)); }
    void U16(u16 value) { Append(&value, sizeof(value)); }
    void U32(u32 value) { Append(&value, sizeof(value)); }
    void Raw(const std::vector<u8>& data) { bytes.insert(bytes.end(), data.begin(), data.end()); }

private:
    void Append(const void* source, usize size) {
        const auto* first = static_cast<const u8*>(source);
        bytes.insert(bytes.end(), first, first + size);
    }
    std::vector<u8> bytes;
};

/// Wraps a JSON chunk and a binary chunk in the GLB container.
[[nodiscard]] std::vector<u8> MakeGlb(std::string_view json, const std::vector<u8>& bin) {
    std::vector<u8> out;
    auto put = [&out](u32 value) {
        u8 raw[4];
        std::memcpy(raw, &value, sizeof(raw));
        out.insert(out.end(), raw, raw + 4);
    };

    std::string paddedJson(json);
    while (paddedJson.size() % 4 != 0) {
        paddedJson += ' '; // The spec pads JSON with spaces.
    }
    std::vector<u8> paddedBin = bin;
    while (paddedBin.size() % 4 != 0) {
        paddedBin.push_back(0);
    }

    const u32 total = 12 + 8 + static_cast<u32>(paddedJson.size()) +
                      (paddedBin.empty() ? 0 : 8 + static_cast<u32>(paddedBin.size()));
    put(0x46546C67U); // "glTF"
    put(2);
    put(total);
    put(static_cast<u32>(paddedJson.size()));
    put(0x4E4F534AU);
    out.insert(out.end(), paddedJson.begin(), paddedJson.end());
    if (!paddedBin.empty()) {
        put(static_cast<u32>(paddedBin.size()));
        put(0x004E4942U);
        out.insert(out.end(), paddedBin.begin(), paddedBin.end());
    }
    return out;
}

/// One triangle with positions, normals, uvs, 32 bit indices and one material.
[[nodiscard]] std::vector<u8> MakeTriangleGlb() {
    BinWriter bin;
    // positions
    bin.F32(0.0f); bin.F32(0.0f); bin.F32(0.0f);
    bin.F32(1.0f); bin.F32(0.0f); bin.F32(0.0f);
    bin.F32(0.0f); bin.F32(2.0f); bin.F32(0.0f);
    // normals
    bin.F32(0.0f); bin.F32(0.0f); bin.F32(1.0f);
    bin.F32(0.0f); bin.F32(0.0f); bin.F32(1.0f);
    bin.F32(0.0f); bin.F32(0.0f); bin.F32(1.0f);
    // uvs
    bin.F32(0.0f); bin.F32(0.0f);
    bin.F32(1.0f); bin.F32(0.0f);
    bin.F32(0.0f); bin.F32(1.0f);
    // indices
    bin.U32(0); bin.U32(1); bin.U32(2);

    const std::string json = R"({
      "asset": {"version": "2.0"},
      "buffers": [{"byteLength": 108}],
      "bufferViews": [
        {"buffer": 0, "byteOffset": 0,  "byteLength": 36},
        {"buffer": 0, "byteOffset": 36, "byteLength": 36},
        {"buffer": 0, "byteOffset": 72, "byteLength": 24},
        {"buffer": 0, "byteOffset": 96, "byteLength": 12}
      ],
      "accessors": [
        {"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3"},
        {"bufferView": 1, "componentType": 5126, "count": 3, "type": "VEC3"},
        {"bufferView": 2, "componentType": 5126, "count": 3, "type": "VEC2"},
        {"bufferView": 3, "componentType": 5125, "count": 3, "type": "SCALAR"}
      ],
      "meshes": [{
        "name": "triangle",
        "primitives": [{
          "attributes": {"POSITION": 0, "NORMAL": 1, "TEXCOORD_0": 2},
          "indices": 3,
          "material": 0
        }]
      }],
      "materials": [{
        "name": "red",
        "doubleSided": true,
        "pbrMetallicRoughness": {
          "baseColorFactor": [1.0, 0.0, 0.0, 0.5],
          "metallicFactor": 0.25,
          "roughnessFactor": 0.75
        }
      }]
    })";
    return MakeGlb(json, bin.Bytes());
}

/// The same triangle, but with all three attributes interleaved in one view.
[[nodiscard]] std::vector<u8> MakeInterleavedGlb() {
    BinWriter bin;
    const f32 positions[9] = {0, 0, 0, 1, 0, 0, 0, 2, 0};
    const f32 uvs[6] = {0, 0, 1, 0, 0, 1};
    for (usize i = 0; i < 3; ++i) {
        bin.F32(positions[i * 3 + 0]);
        bin.F32(positions[i * 3 + 1]);
        bin.F32(positions[i * 3 + 2]);
        bin.F32(0.0f); bin.F32(1.0f); bin.F32(0.0f); // normal
        bin.F32(uvs[i * 2 + 0]);
        bin.F32(uvs[i * 2 + 1]);
    }
    bin.U16(0); bin.U16(1); bin.U16(2); // 16 bit indices, 6 bytes

    const std::string json = R"({
      "asset": {"version": "2.0"},
      "buffers": [{"byteLength": 104}],
      "bufferViews": [
        {"buffer": 0, "byteOffset": 0, "byteLength": 96, "byteStride": 32},
        {"buffer": 0, "byteOffset": 96, "byteLength": 6}
      ],
      "accessors": [
        {"bufferView": 0, "byteOffset": 0,  "componentType": 5126, "count": 3, "type": "VEC3"},
        {"bufferView": 0, "byteOffset": 12, "componentType": 5126, "count": 3, "type": "VEC3"},
        {"bufferView": 0, "byteOffset": 24, "componentType": 5126, "count": 3, "type": "VEC2"},
        {"bufferView": 1, "componentType": 5123, "count": 3, "type": "SCALAR"}
      ],
      "meshes": [{"primitives": [{
        "attributes": {"POSITION": 0, "NORMAL": 1, "TEXCOORD_0": 2},
        "indices": 3
      }]}]
    })";
    return MakeGlb(json, bin.Bytes());
}

/// Positions and indices only: normals and uvs have to be generated.
[[nodiscard]] std::vector<u8> MakeNoNormalsGlb() {
    BinWriter bin;
    bin.F32(0.0f); bin.F32(0.0f); bin.F32(0.0f);
    bin.F32(1.0f); bin.F32(0.0f); bin.F32(0.0f);
    bin.F32(0.0f); bin.F32(1.0f); bin.F32(0.0f);
    bin.U32(0); bin.U32(1); bin.U32(2);

    const std::string json = R"({
      "asset": {"version": "2.0"},
      "buffers": [{"byteLength": 48}],
      "bufferViews": [
        {"buffer": 0, "byteOffset": 0,  "byteLength": 36},
        {"buffer": 0, "byteOffset": 36, "byteLength": 12}
      ],
      "accessors": [
        {"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3"},
        {"bufferView": 1, "componentType": 5125, "count": 3, "type": "SCALAR"}
      ],
      "meshes": [{"name": "bare", "primitives": [{
        "attributes": {"POSITION": 0}, "indices": 1
      }]}]
    })";
    return MakeGlb(json, bin.Bytes());
}

/// A triangle strip primitive, which the importer must skip with a warning.
[[nodiscard]] std::vector<u8> MakeStripGlb() {
    BinWriter bin;
    bin.F32(0.0f); bin.F32(0.0f); bin.F32(0.0f);
    bin.F32(1.0f); bin.F32(0.0f); bin.F32(0.0f);
    bin.F32(0.0f); bin.F32(1.0f); bin.F32(0.0f);

    const std::string json = R"({
      "asset": {"version": "2.0"},
      "buffers": [{"byteLength": 36}],
      "bufferViews": [{"buffer": 0, "byteOffset": 0, "byteLength": 36}],
      "accessors": [{"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3"}],
      "meshes": [{"name": "strip", "primitives": [
        {"attributes": {"POSITION": 0}, "mode": 5},
        {"attributes": {"POSITION": 0}}
      ]}]
    })";
    return MakeGlb(json, bin.Bytes());
}

/// A GLB whose image lives in the bin chunk and whose material uses it.
[[nodiscard]] std::vector<u8> MakeEmbeddedTextureGlb(const std::vector<u8>& png) {
    BinWriter bin;
    bin.F32(0.0f); bin.F32(0.0f); bin.F32(0.0f);
    bin.F32(1.0f); bin.F32(0.0f); bin.F32(0.0f);
    bin.F32(0.0f); bin.F32(1.0f); bin.F32(0.0f);
    bin.U32(0); bin.U32(1); bin.U32(2);
    const u32 imageOffset = bin.Offset();
    bin.Raw(png);

    std::string json = R"({
      "asset": {"version": "2.0"},
      "buffers": [{"byteLength": )" + std::to_string(bin.Offset()) + R"(}],
      "bufferViews": [
        {"buffer": 0, "byteOffset": 0,  "byteLength": 36},
        {"buffer": 0, "byteOffset": 36, "byteLength": 12},
        {"buffer": 0, "byteOffset": )" + std::to_string(imageOffset) +
                       R"(, "byteLength": )" + std::to_string(png.size()) + R"(}
      ],
      "accessors": [
        {"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3"},
        {"bufferView": 1, "componentType": 5125, "count": 3, "type": "SCALAR"}
      ],
      "images": [{"bufferView": 2, "mimeType": "image/png", "name": "albedo"}],
      "textures": [{"source": 0}],
      "materials": [{
        "name": "textured",
        "pbrMetallicRoughness": {
          "baseColorFactor": [1.0, 1.0, 1.0, 1.0],
          "baseColorTexture": {"index": 0},
          "metallicFactor": 0.0,
          "roughnessFactor": 0.9
        }
      }],
      "meshes": [{"name": "quad", "primitives": [{
        "attributes": {"POSITION": 0}, "indices": 1, "material": 0
      }]}]
    })";
    return MakeGlb(json, bin.Bytes());
}

// --- Image and audio inputs ------------------------------------------------

[[nodiscard]] std::vector<u8> MakePng(u32 width, u32 height, const std::vector<u8>& rgba) {
    std::vector<u8> out;
    stbi_write_png_to_func(&AppendCallback, &out, static_cast<int>(width),
                           static_cast<int>(height), 4, rgba.data(),
                           static_cast<int>(width * 4));
    return out;
}

[[nodiscard]] std::vector<u8> MakeHdr(u32 width, u32 height, const std::vector<f32>& rgb) {
    std::vector<u8> out;
    stbi_write_hdr_to_func(&AppendCallback, &out, static_cast<int>(width),
                           static_cast<int>(height), 3, rgb.data());
    return out;
}

/// A solid colour image, used for the image importer tests.
[[nodiscard]] std::vector<u8> SolidRgba(u32 width, u32 height, u8 r, u8 g, u8 b, u8 a) {
    std::vector<u8> pixels(static_cast<usize>(width) * height * 4);
    for (usize i = 0; i < pixels.size(); i += 4) {
        pixels[i + 0] = r;
        pixels[i + 1] = g;
        pixels[i + 2] = b;
        pixels[i + 3] = a;
    }
    return pixels;
}

/// Builds a PCM or float WAVE file.  `extraOddChunk` inserts a chunk of odd
/// length so the word alignment rule is exercised.
[[nodiscard]] std::vector<u8> MakeWav(u16 formatTag, u16 channels, u32 sampleRate, u16 bits,
                                      const std::vector<u8>& data, bool extraOddChunk = false) {
    BinWriter body;
    // fmt chunk
    body.U16(formatTag);
    body.U16(channels);
    body.U32(sampleRate);
    body.U32(sampleRate * channels * static_cast<u32>(bits / 8));
    body.U16(static_cast<u16>(channels * (bits / 8)));
    body.U16(bits);

    std::vector<u8> out;
    auto put = [&out](u32 value) {
        u8 raw[4];
        std::memcpy(raw, &value, sizeof(raw));
        out.insert(out.end(), raw, raw + 4);
    };
    auto putTag = [&out](const char* tag) { out.insert(out.end(), tag, tag + 4); };

    const u32 riffSize = 4 + (8 + 16) + (extraOddChunk ? 8 + 3 + 1 : 0) + (8 + static_cast<u32>(data.size()));
    putTag("RIFF");
    put(riffSize);
    putTag("WAVE");

    putTag("fmt ");
    put(16);
    out.insert(out.end(), body.Bytes().begin(), body.Bytes().end());

    if (extraOddChunk) {
        putTag("LIST");
        put(3);
        out.push_back('a');
        out.push_back('b');
        out.push_back('c');
        out.push_back(0); // Pad byte, not counted in the chunk size.
    }

    putTag("data");
    put(static_cast<u32>(data.size()));
    out.insert(out.end(), data.begin(), data.end());
    return out;
}

// --- Pipeline fixtures -----------------------------------------------------

/// An asset root with one mesh, one texture and one sound, in memory.
struct ProjectFixture {
    MemoryFileSystem source;
    MemoryFileSystem cooked;
    ImporterRegistry importers = CreateDefaultImporters();
    std::vector<u8> glb;
    std::vector<u8> png;
    std::vector<u8> wav;

    ProjectFixture() {
        glb = MakeTriangleGlb();
        png = MakePng(4, 4, SolidRgba(4, 4, 255, 0, 0, 255));
        std::vector<u8> samples;
        BinWriter writer;
        writer.U16(16384);  // +0.5
        writer.U16(49152);  // -0.5 (as unsigned storage of -16384)
        writer.U16(0);      // silence
        writer.U16(32768);  // -1.0
        samples = writer.Bytes();
        wav = MakeWav(1, 1, 8000, 16, samples);

        REQUIRE(source.WriteFile("models/cube.glb", Bytes(glb)).HasValue());
        REQUIRE(source.WriteFile("textures/albedo.png", Bytes(png)).HasValue());
        REQUIRE(source.WriteFile("audio/ping.wav", Bytes(wav)).HasValue());
    }

    [[nodiscard]] AssetDatabase MakeDatabase() {
        AssetDatabase database;
        auto initialized = database.Initialize(source, AssetDatabaseDesc{});
        REQUIRE(initialized.HasValue());
        return database;
    }
};

} // namespace

// ===========================================================================
TEST_SUITE("assets.identity") {
    TEST_CASE("asset paths normalise to a canonical form") {
        CHECK(AssetPath{"models/cube.glb"}.Text() == "models/cube.glb");
        CHECK(AssetPath{"models\\cube.glb"}.Text() == "models/cube.glb");
        CHECK(AssetPath{"./models//cube.glb"}.Text() == "models/cube.glb");
        CHECK(AssetPath{"/models/cube.glb/"}.Text() == "models/cube.glb");
        CHECK(AssetPath{"models/./cube.glb"}.Text() == "models/cube.glb");
        CHECK_FALSE(AssetPath{"../escape.glb"}.IsValid());
        CHECK_FALSE(AssetPath{"models/../../escape.glb"}.IsValid());
        CHECK(AssetPath::Create("../escape.glb").IsError());
        CHECK(AssetPath::Create("models/cube.glb").HasValue());
    }

    TEST_CASE("asset paths split into their parts") {
        const AssetPath path{"models/sub/cube.glb"};
        CHECK(path.FileName() == "cube.glb");
        CHECK(path.Stem() == "cube");
        CHECK(path.Extension() == ".glb");
        CHECK(path.Parent().Text() == "models/sub");
        CHECK(path.Parent().Parent().Text() == "models");
        CHECK_FALSE(path.Parent().Parent().Parent().IsValid());
        CHECK(path.Child("extra.txt").Text() == "models/sub/cube.glb/extra.txt");
        CHECK(path.Appending(".l3dmeta").Text() == "models/sub/cube.glb.l3dmeta");
        CHECK(path.WithExtension(".obj").Text() == "models/sub/cube.obj");
        CHECK(path.WithExtension("obj").Text() == "models/sub/cube.obj");
        CHECK(MetaPathFor(path).Text() == "models/sub/cube.glb.l3dmeta");
    }

    TEST_CASE("extensions are case insensitive, paths compare case insensitively") {
        const AssetPath upper{"Models/Cube.GLB"};
        CHECK(upper.Extension() == ".glb");
        CHECK(upper.Type() == AssetType::Mesh);
        CHECK(upper == AssetPath{"models/cube.glb"});
        CHECK(upper.Hash() == AssetPath{"models/cube.glb"}.Hash());
        // Ordering stays case sensitive so a conflict is visible as two keys.
        CHECK(upper.Text() != AssetPath{"models/cube.glb"}.Text());
    }

    TEST_CASE("a dot file has no extension") {
        const AssetPath path{".gitignore"};
        CHECK(path.Extension().empty());
        CHECK(path.Stem() == ".gitignore");
        CHECK(path.Type() == AssetType::Unknown);
    }

    TEST_CASE("asset ids are stable by name and distinct per sub asset") {
        const AssetId a = AssetId::FromName("builtin/white");
        const AssetId b = AssetId::FromName("builtin/white");
        const AssetId c = AssetId::FromName("builtin/black");
        CHECK(a == b);
        CHECK(a != c);
        CHECK_FALSE(a.IsNull());
        CHECK(AssetId{}.IsNull());

        const AssetId mesh0 = a.Sub(0);
        const AssetId mesh1 = a.Sub(1);
        CHECK(mesh0 == a.Sub(0));
        CHECK(mesh0 != mesh1);
        CHECK(mesh0 != a);
        CHECK(c.Sub(1) != mesh1);
    }

    TEST_CASE("asset ids round trip through text") {
        const AssetId id = AssetId::FromName("models/cube");
        const std::string text = id.ToString();
        CHECK(text.size() == 36);
        auto parsed = AssetId::Parse(text);
        REQUIRE(parsed.HasValue());
        CHECK(*parsed == id);
        CHECK(AssetId::Parse("not-an-id").IsError());
        CHECK(AssetId::Parse("").IsError());
    }

    TEST_CASE("types map to and from their names and extensions") {
        CHECK(AssetTypeFromString("Mesh") == AssetType::Mesh);
        CHECK(AssetTypeFromString("Texture") == AssetType::Texture);
        CHECK(AssetTypeFromString("AudioClip") == AssetType::AudioClip);
        CHECK(AssetTypeFromString("nonsense") == AssetType::Unknown);
        CHECK(AssetTypeFromExtension(".glb") == AssetType::Mesh);
        CHECK(AssetTypeFromExtension("glb") == AssetType::Mesh);
        CHECK(AssetTypeFromExtension(".PNG") == AssetType::Texture);
        CHECK(AssetTypeFromExtension(".wav") == AssetType::AudioClip);
        CHECK(AssetTypeFromExtension(".l3dmat") == AssetType::Material);
        CHECK(AssetTypeFromExtension(".exe") == AssetType::Unknown);
        CHECK(std::string(AssetTypeToString(AssetType::Mesh)) == "Mesh");
    }
}

TEST_SUITE("assets.filesystem") {
    TEST_CASE("the memory file system reads, writes and lists") {
        MemoryFileSystem fs;
        const std::vector<u8> data = {1, 2, 3, 4};
        CHECK(fs.ReadFile("missing.bin").IsError());
        CHECK(fs.ReadFile("missing.bin").Error().Code() == StatusCode::NotFound);

        REQUIRE(fs.WriteFile("a/b/file.bin", Bytes(data)).HasValue());
        auto read = fs.ReadFile("a/b/file.bin");
        REQUIRE(read.HasValue());
        CHECK(*read == data);
        CHECK(fs.Exists("a/b/file.bin"));
        CHECK(fs.IsDirectory("a/b"));
        CHECK(fs.IsDirectory("a"));
        CHECK(fs.IsDirectory(""));
        CHECK_FALSE(fs.IsDirectory("a/b/file.bin"));
        CHECK(fs.FileSize("a/b/file.bin") == 4);

        auto listed = fs.ListDirectory("a/b");
        REQUIRE(listed.HasValue());
        CHECK(listed->size() == 1);
        CHECK((*listed)[0] == "file.bin");

        auto roots = fs.ListDirectory("");
        REQUIRE(roots.HasValue());
        CHECK(roots->size() == 1);
        CHECK((*roots)[0] == "a");

        // A file whose name is a prefix of a directory name must not fool the
        // directory check.
        REQUIRE(fs.WriteFile("models.txt", Bytes(data)).HasValue());
        REQUIRE(fs.WriteFile("models/cube.glb", Bytes(data)).HasValue());
        CHECK(fs.IsDirectory("models"));

        REQUIRE(fs.Remove("a/b/file.bin").HasValue());
        CHECK_FALSE(fs.Exists("a/b/file.bin"));
        CHECK(fs.ReadFile("a/b/file.bin").IsError());
        CHECK(fs.FileCount() == 2);
    }

    TEST_CASE("paths that escape the root are rejected") {
        MemoryFileSystem fs;
        CHECK(fs.ReadFile("../etc/passwd").IsError());
        CHECK(fs.WriteFile("../escape.bin", Bytes({1})).IsError());
        CHECK_FALSE(fs.Exists("../escape.bin"));
        CHECK(fs.ListDirectory("../..").IsError());
    }

    TEST_CASE("explicitly created directories are listed") {
        MemoryFileSystem fs;
        REQUIRE(fs.CreateDirectories("empty/dir").HasValue());
        CHECK(fs.IsDirectory("empty/dir"));
        auto listed = fs.ListDirectory("empty");
        REQUIRE(listed.HasValue());
        CHECK(listed->size() == 1);
        CHECK((*listed)[0] == "dir");
    }

    TEST_CASE("path normalisation rejects traversal") {
        std::string out;
        CHECK(NormalizePath("a/./b//c/", out));
        CHECK(out == "a/b/c");
        CHECK(NormalizePath("a\\b", out));
        CHECK(out == "a/b");
        CHECK_FALSE(NormalizePath("a/../b", out));
        CHECK(out.empty());
    }
}

TEST_SUITE("assets.meta") {
    TEST_CASE("a sidecar round trips through json") {
        AssetMeta meta;
        meta.id = AssetId::FromName("models/cube");
        meta.type = AssetType::Mesh;
        meta.importer = "glb";
        meta.importerVersion = 7;
        meta.cookedSourceHash = 0xDEADBEEFCAFEBABEULL;
        meta.cookedImportHash = 0xFFFFFFFFFFFFFFFFULL;
        meta.settings.Set("srgb", false);

        const std::string text = meta.Dump();
        auto parsed = AssetMeta::Parse(text);
        REQUIRE(parsed.HasValue());
        CHECK(parsed->id == meta.id);
        CHECK(parsed->type == AssetType::Mesh);
        CHECK(parsed->importer == "glb");
        CHECK(parsed->importerVersion == 7);
        // 64 bit hashes must survive: JSON numbers are doubles.
        CHECK(parsed->cookedSourceHash == 0xDEADBEEFCAFEBABEULL);
        CHECK(parsed->cookedImportHash == 0xFFFFFFFFFFFFFFFFULL);
        CHECK_FALSE(parsed->settings["srgb"].AsBool(true));
        CHECK(parsed->formatVersion == AssetMeta::kCurrentFormatVersion);
    }

    TEST_CASE("a malformed sidecar is an error, not a crash") {
        CHECK(AssetMeta::Parse("not json").IsError());
        CHECK(AssetMeta::Parse("[]").IsError());
        CHECK(AssetMeta::Parse("{}").IsError()); // no id
        CHECK(AssetMeta::Parse(R"({"id": "nonsense"})").IsError());
        CHECK(AssetMeta::Parse(R"({"id": "00000000-0000-0000-0000-000000000000"})").HasValue());
    }

    TEST_CASE("the import fingerprint moves when the settings move") {
        AssetMeta meta;
        meta.importer = "image";
        meta.importerVersion = 1;
        const u64 before = meta.ImportFingerprint();
        meta.settings.Set("maxSize", 512);
        const u64 after = meta.ImportFingerprint();
        CHECK(before != after);
        meta.importerVersion = 2;
        CHECK(after != meta.ImportFingerprint());
        // The record and the meta must agree, or cooking would thrash.
        CHECK(ImportFingerprintOf(meta.importer, meta.importerVersion,
                                  HashSettings(meta.settings)) == meta.ImportFingerprint());
    }

    TEST_CASE("texture settings survive the json round trip") {
        TextureImportSettings settings;
        settings.srgb = false;
        settings.generateMips = false;
        settings.maxSize = 256;
        settings.alphaIsCoverage = true;

        const serial::JsonValue json = TextureSettingsToJson(settings);
        const TextureImportSettings back = TextureSettingsFromJson(json);
        CHECK_FALSE(back.srgb);
        CHECK_FALSE(back.generateMips);
        CHECK(back.maxSize == 256);
        CHECK(back.alphaIsCoverage);
        // The settings hash is what makes cooking incremental, so it has to be
        // stable and sensitive to every field.
        CHECK(HashSettings(json) == HashSettings(TextureSettingsToJson(back)));
        TextureImportSettings other = settings;
        other.maxSize = 512;
        CHECK(HashSettings(json) != HashSettings(TextureSettingsToJson(other)));
        CHECK(settings.Hash() != other.Hash());
    }

    TEST_CASE("sidecars are written next to their source") {
        MemoryFileSystem fs;
        REQUIRE(fs.WriteFile("models/cube.glb", Bytes({1, 2, 3})).HasValue());
        CHECK(AssetMeta::Load(fs, AssetPath{"models/cube.glb"}.Appending("")).IsError());

        AssetMeta meta;
        meta.id = AssetId::FromName("models/cube");
        meta.type = AssetType::Mesh;
        meta.importer = "glb";
        REQUIRE(meta.Save(fs, AssetPath{"models/cube.glb"}).HasValue());
        CHECK(fs.Exists("models/cube.glb.l3dmeta"));

        auto loaded = AssetMeta::Load(fs, AssetPath{"models/cube.glb"});
        REQUIRE(loaded.HasValue());
        CHECK(loaded->id == meta.id);
    }
}

TEST_SUITE("assets.glb") {
    TEST_CASE("a triangle imports with all of its attributes") {
        const std::vector<u8> glb = MakeTriangleGlb();
        auto importer = CreateGlbImporter();
        CHECK(importer->CanImport(AssetPath{"models/cube.glb"}));
        CHECK_FALSE(importer->CanImport(AssetPath{"models/cube.gltf"}));

        ImportLog log;
        auto imported = importer->Import(Bytes(glb), serial::JsonValue::MakeObject(),
                                        AssetPath{"models/cube.glb"}, log);
        REQUIRE(imported.HasValue());
        REQUIRE(std::holds_alternative<MeshDocument>(*imported));
        const MeshDocument& document = std::get<MeshDocument>(*imported);
        CHECK(document.name == "cube");
        REQUIRE(document.meshes.size() == 1);

        const MeshData& mesh = document.meshes[0];
        CHECK(mesh.name == "triangle");
        CHECK(mesh.positions.size() == 3);
        CHECK(mesh.normals.size() == 3);
        CHECK(mesh.uvs.size() == 3);
        CHECK(mesh.indices.size() == 3);
        CHECK(mesh.IsValid());
        CHECK(mesh.positions[1].x == doctest::Approx(1.0f));
        CHECK(mesh.positions[2].y == doctest::Approx(2.0f));
        CHECK(mesh.normals[0].z == doctest::Approx(1.0f));
        CHECK(mesh.uvs[2].y == doctest::Approx(1.0f));
        CHECK(mesh.indices[2] == 2);
        CHECK(mesh.materialIndex == 0);
        CHECK(mesh.bounds.min.x == doctest::Approx(0.0f));
        CHECK(mesh.bounds.max.x == doctest::Approx(1.0f));
        CHECK(mesh.bounds.max.y == doctest::Approx(2.0f));

        REQUIRE(document.materials.size() == 1);
        const MaterialData& material = document.materials[0];
        CHECK(material.name == "red");
        CHECK(material.baseColor.x == doctest::Approx(1.0f));
        CHECK(material.baseColor.w == doctest::Approx(0.5f));
        CHECK(material.metallic == doctest::Approx(0.25f));
        CHECK(material.roughness == doctest::Approx(0.75f));
        CHECK(material.doubleSided);
        CHECK(log.WarningCount() == 0);
    }

    TEST_CASE("interleaved attributes and 16 bit indices are handled") {
        const std::vector<u8> glb = MakeInterleavedGlb();
        auto importer = CreateGlbImporter();
        ImportLog log;
        auto imported = importer->Import(Bytes(glb), serial::JsonValue::MakeObject(),
                                        AssetPath{"models/interleaved.glb"}, log);
        REQUIRE(imported.HasValue());
        const MeshDocument& document = std::get<MeshDocument>(*imported);
        REQUIRE(document.meshes.size() == 1);
        const MeshData& mesh = document.meshes[0];
        CHECK(mesh.positions.size() == 3);
        // The stride is 32 bytes, so the normals start at byte 12.
        CHECK(mesh.normals[0].y == doctest::Approx(1.0f));
        CHECK(mesh.normals[0].x == doctest::Approx(0.0f));
        CHECK(mesh.uvs[1].x == doctest::Approx(1.0f));
        CHECK(mesh.indices[2] == 2);
        // An unnamed mesh gets a deterministic name.
        CHECK(mesh.name == "mesh_0");
    }

    TEST_CASE("missing normals are generated and missing uvs are zeroed") {
        const std::vector<u8> glb = MakeNoNormalsGlb();
        auto importer = CreateGlbImporter();
        ImportLog log;
        auto imported = importer->Import(Bytes(glb), serial::JsonValue::MakeObject(),
                                        AssetPath{"models/bare.glb"}, log);
        REQUIRE(imported.HasValue());
        const MeshDocument& document = std::get<MeshDocument>(*imported);
        const MeshData& mesh = document.meshes[0];
        CHECK(mesh.normals.size() == 3);
        // The triangle lies in the XY plane, wound counter clockwise, so the
        // generated normal points down +Z.
        CHECK(mesh.normals[0].z == doctest::Approx(1.0f));
        CHECK(mesh.normals[0].x == doctest::Approx(0.0f));
        CHECK(std::fabs(math::Length(mesh.normals[0]) - 1.0f) < 1e-5f);
        CHECK(mesh.uvs.size() == 3);
        CHECK(mesh.uvs[0].x == doctest::Approx(0.0f));
        CHECK(log.WarningCount() == 2);
    }

    TEST_CASE("non triangle primitives are skipped with a warning") {
        const std::vector<u8> glb = MakeStripGlb();
        auto importer = CreateGlbImporter();
        ImportLog log;
        auto imported = importer->Import(Bytes(glb), serial::JsonValue::MakeObject(),
                                        AssetPath{"models/strip.glb"}, log);
        REQUIRE(imported.HasValue());
        const MeshDocument& document = std::get<MeshDocument>(*imported);
        // The strip was dropped, the triangle list kept.
        CHECK(document.meshes.size() == 1);
        // One for the skipped strip, plus generated normals and zeroed uvs for
        // the primitive that was kept.
        CHECK(log.WarningCount() == 3);
        CHECK(log.Warnings()[0].find("not a triangle list") != std::string::npos);
    }

    TEST_CASE("broken containers are rejected with a useful status") {
        auto importer = CreateGlbImporter();
        ImportLog log;
        const AssetPath path{"models/bad.glb"};

        auto tiny = importer->Import(Bytes({1, 2, 3}), serial::JsonValue::MakeObject(), path, log);
        CHECK(tiny.Error().Code() == StatusCode::ParseError);

        const std::vector<u8> notGlb = MakeGlb(R"({"asset":{"version":"2.0"}})", {});
        std::vector<u8> badMagic = notGlb;
        badMagic[0] = 'X';
        auto magic = importer->Import(Bytes(badMagic), serial::JsonValue::MakeObject(), path, log);
        CHECK(magic.IsError());
        CHECK(magic.Error().Message().find("GLB") != std::string::npos);

        std::vector<u8> badVersion = notGlb;
        badVersion[4] = 3;
        auto version = importer->Import(Bytes(badVersion), serial::JsonValue::MakeObject(), path, log);
        CHECK(version.Error().Code() == StatusCode::Unsupported);

        // A truncated chunk must not read past the end.
        std::vector<u8> truncated = MakeTriangleGlb();
        truncated.resize(truncated.size() - 40);
        auto cut = importer->Import(Bytes(truncated), serial::JsonValue::MakeObject(), path, log);
        CHECK(cut.IsError());

        // An accessor that runs past the bin chunk.
        const std::string json = R"({
          "asset": {"version": "2.0"},
          "buffers": [{"byteLength": 12}],
          "bufferViews": [{"buffer": 0, "byteOffset": 0, "byteLength": 12}],
          "accessors": [{"bufferView": 0, "componentType": 5126, "count": 999, "type": "VEC3"}],
          "meshes": [{"primitives": [{"attributes": {"POSITION": 0}}]}]
        })";
        const std::vector<u8> overflow = MakeGlb(json, std::vector<u8>(12, 0));
        auto past = importer->Import(Bytes(overflow), serial::JsonValue::MakeObject(), path, log);
        CHECK(past.Error().Code() == StatusCode::ParseError);
    }

    TEST_CASE("a gltf 1 asset is refused") {
        auto importer = CreateGlbImporter();
        ImportLog log;
        const std::vector<u8> glb = MakeGlb(R"({"asset": {"version": "1.0"}})", {});
        auto imported = importer->Import(Bytes(glb), serial::JsonValue::MakeObject(),
                                        AssetPath{"models/old.glb"}, log);
        CHECK(imported.Error().Code() == StatusCode::Unsupported);
    }

    TEST_CASE("an index that points past the vertex list is an error") {
        BinWriter bin;
        bin.F32(0.0f); bin.F32(0.0f); bin.F32(0.0f);
        bin.F32(1.0f); bin.F32(0.0f); bin.F32(0.0f);
        bin.F32(0.0f); bin.F32(1.0f); bin.F32(0.0f);
        bin.U32(0); bin.U32(1); bin.U32(9);
        const std::string json = R"({
          "asset": {"version": "2.0"},
          "buffers": [{"byteLength": 48}],
          "bufferViews": [
            {"buffer": 0, "byteOffset": 0, "byteLength": 36},
            {"buffer": 0, "byteOffset": 36, "byteLength": 12}
          ],
          "accessors": [
            {"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3"},
            {"bufferView": 1, "componentType": 5125, "count": 3, "type": "SCALAR"}
          ],
          "meshes": [{"primitives": [{"attributes": {"POSITION": 0}, "indices": 1}]}]
        })";
        const std::vector<u8> glb = MakeGlb(json, bin.Bytes());
        auto importer = CreateGlbImporter();
        ImportLog log;
        auto imported = importer->Import(Bytes(glb), serial::JsonValue::MakeObject(),
                                        AssetPath{"models/bad-index.glb"}, log);
        CHECK(imported.IsError());
        CHECK(imported.Error().Message().find("missing vertex") != std::string::npos);
    }

    TEST_CASE("embedded images come out with the file they were stored in") {
        const std::vector<u8> png = MakePng(2, 2, SolidRgba(2, 2, 0, 255, 0, 255));
        const std::vector<u8> glb = MakeEmbeddedTextureGlb(png);
        auto importer = CreateGlbImporter();
        ImportLog log;
        auto imported = importer->Import(Bytes(glb), serial::JsonValue::MakeObject(),
                                        AssetPath{"models/textured.glb"}, log);
        REQUIRE(imported.HasValue());
        const MeshDocument& document = std::get<MeshDocument>(*imported);
        REQUIRE(document.embeddedImages.size() == 1);
        CHECK(document.embeddedImages[0].name == "albedo");
        CHECK(document.embeddedImages[0].mimeType == "image/png");
        CHECK(document.embeddedImages[0].bytes.size() == png.size());
        REQUIRE(document.materials.size() == 1);
        CHECK(document.materials[0].baseColorTexture == "embedded:albedo");
        CHECK(document.FindEmbeddedImage("albedo") == 0);
        CHECK(document.FindEmbeddedImage("nope") == -1);
    }
}

TEST_SUITE("assets.image") {
    TEST_CASE("a png imports with a full mip chain") {
        const std::vector<u8> png = MakePng(4, 4, SolidRgba(4, 4, 255, 128, 0, 255));
        REQUIRE(png.size() > 8);
        auto importer = CreateImageImporter();
        CHECK(importer->CanImport(AssetPath{"t/a.png"}));
        CHECK(importer->CanImport(AssetPath{"t/a.PNG"}));
        CHECK(importer->CanImport(AssetPath{"t/a.jpg"}));
        CHECK_FALSE(importer->CanImport(AssetPath{"t/a.wav"}));

        ImportLog log;
        serial::JsonValue settings = TextureSettingsToJson(TextureImportSettings{});
        auto imported = importer->Import(Bytes(png), settings, AssetPath{"t/a.png"}, log);
        REQUIRE(imported.HasValue());
        const TextureDocument& document = std::get<TextureDocument>(*imported);
        const ImageData& image = document.image;
        CHECK(image.width == 4);
        CHECK(image.height == 4);
        CHECK(image.mipLevels == 3); // 4x4, 2x2, 1x1
        CHECK(image.format == rhi::Format::RGBA8_SRGB);
        CHECK(image.BytesPerPixel() == 4);
        CHECK(image.MipBytes(0) == 64);
        CHECK(image.MipBytes(1) == 16);
        CHECK(image.MipBytes(2) == 4);
        CHECK(image.MipOffset(1) == 64);
        CHECK(image.MipOffset(2) == 80);
        CHECK(image.pixels.size() == 84);
        CHECK(image.IsValid());
    }

    TEST_CASE("a data texture is stored unorm and keeps its encoding") {
        const std::vector<u8> png = MakePng(2, 2, SolidRgba(2, 2, 200, 100, 50, 255));
        auto importer = CreateImageImporter();
        ImportLog log;
        TextureImportSettings settings;
        settings.srgb = false;
        settings.generateMips = false;
        auto imported = importer->Import(Bytes(png), TextureSettingsToJson(settings),
                                        AssetPath{"t/normal.png"}, log);
        REQUIRE(imported.HasValue());
        const ImageData& image = std::get<TextureDocument>(*imported).image;
        CHECK(image.format == rhi::Format::RGBA8_UNorm);
        CHECK(image.mipLevels == 1);
        CHECK(image.pixels.size() == 16);
        // No transfer function was applied, so the bytes are untouched.
        CHECK(image.pixels[0] == 200);
        CHECK(image.pixels[1] == 100);
        CHECK(image.pixels[2] == 50);
    }

    TEST_CASE("the generated mip is the box filtered average") {
        // A 2x2 image whose four texels differ only in red: 0, 100, 200, 255.
        std::vector<u8> pixels = {0, 0, 0, 255, 100, 0, 0, 255, 200, 0, 0, 255, 255, 0, 0, 255};
        const std::vector<u8> png = MakePng(2, 2, pixels);
        auto importer = CreateImageImporter();
        ImportLog log;
        TextureImportSettings settings;
        settings.srgb = false; // Filter in the stored space to keep this exact.
        auto imported = importer->Import(Bytes(png), TextureSettingsToJson(settings),
                                        AssetPath{"t/ramp.png"}, log);
        REQUIRE(imported.HasValue());
        const ImageData& image = std::get<TextureDocument>(*imported).image;
        REQUIRE(image.mipLevels == 2);
        CHECK(image.pixels.size() == 20);
        // The 1x1 mip is the mean of 0, 100, 200 and 255, rounded.
        CHECK(image.pixels[16] == 139);
        CHECK(image.pixels[17] == 0);
        CHECK(image.pixels[19] == 255);
    }

    TEST_CASE("maxSize downscales the base mip") {
        const std::vector<u8> png = MakePng(8, 8, SolidRgba(8, 8, 10, 20, 30, 255));
        auto importer = CreateImageImporter();
        ImportLog log;
        TextureImportSettings settings;
        settings.maxSize = 2;
        auto imported = importer->Import(Bytes(png), TextureSettingsToJson(settings),
                                        AssetPath{"t/big.png"}, log);
        REQUIRE(imported.HasValue());
        const ImageData& image = std::get<TextureDocument>(*imported).image;
        CHECK(image.width == 2);
        CHECK(image.height == 2);
        CHECK(image.mipLevels == 2);
    }

    TEST_CASE("an hdr environment map imports as half float") {
        // Two texels of known radiance.
        const std::vector<f32> rgb = {2.0f, 0.0f, 0.0f, 0.0f, 0.5f, 0.0f};
        const std::vector<u8> hdr = MakeHdr(2, 1, rgb);
        REQUIRE(hdr.size() > 16);
        auto importer = CreateImageImporter();
        CHECK(importer->CanImport(AssetPath{"t/sky.hdr"}));
        ImportLog log;
        TextureImportSettings settings;
        settings.generateMips = false;
        auto imported = importer->Import(Bytes(hdr), TextureSettingsToJson(settings),
                                        AssetPath{"t/sky.hdr"}, log);
        REQUIRE(imported.HasValue());
        const ImageData& image = std::get<TextureDocument>(*imported).image;
        CHECK(image.format == rhi::Format::RGBA16_Float);
        CHECK(image.BytesPerPixel() == 8);
        REQUIRE(image.pixels.size() == 16);

        u16 redHalf = 0;
        std::memcpy(&redHalf, image.pixels.data(), sizeof(redHalf));
        // 2.0 in half precision is 0x4000.
        CHECK(redHalf == 0x4000);
        // The second texel's green channel holds 0.5 -> 0x3800.
        u16 greenHalf = 0;
        std::memcpy(&greenHalf, image.pixels.data() + 8 + 2, sizeof(greenHalf));
        CHECK(greenHalf == 0x3800);
    }

    TEST_CASE("undecodable bytes are an error, not a crash") {
        auto importer = CreateImageImporter();
        ImportLog log;
        const std::vector<u8> junk = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
        auto imported = importer->Import(Bytes(junk), TextureSettingsToJson(TextureImportSettings{}),
                                        AssetPath{"t/broken.png"}, log);
        CHECK(imported.IsError());
        CHECK(imported.Error().Code() == StatusCode::ParseError);
    }

    TEST_CASE("mip chain length matches the layout helper") {
        CHECK(MipChainLength(1, 1) == 1);
        CHECK(MipChainLength(2, 2) == 2);
        CHECK(MipChainLength(4, 4) == 3);
        CHECK(MipChainLength(512, 512) == 10);
        CHECK(MipChainLength(8, 4) == 4); // Driven by the longer side.
    }
}

TEST_SUITE("assets.audio") {
    TEST_CASE("a 16 bit mono wav becomes float frames") {
        BinWriter samples;
        samples.U16(0);      // silence
        samples.U16(16384);  // +0.5
        samples.U16(32768);  // -1.0
        const std::vector<u8> wav = MakeWav(1, 1, 48000, 16, samples.Bytes());

        auto importer = CreateWavImporter();
        CHECK(importer->CanImport(AssetPath{"a/ping.wav"}));
        CHECK_FALSE(importer->CanImport(AssetPath{"a/ping.ogg"}));
        ImportLog log;
        auto imported = importer->Import(Bytes(wav), serial::JsonValue::MakeObject(),
                                        AssetPath{"a/ping.wav"}, log);
        REQUIRE(imported.HasValue());
        const AudioData& audio = std::get<AudioDocument>(*imported).audio;
        CHECK(audio.name == "ping");
        CHECK(audio.sampleRate == 48000);
        CHECK(audio.channels == 1);
        CHECK(audio.frameCount == 3);
        CHECK(audio.samples.size() == 3);
        CHECK(audio.IsValid());
        CHECK(audio.samples[0] == doctest::Approx(0.0f));
        CHECK(audio.samples[1] == doctest::Approx(0.5f));
        CHECK(audio.samples[2] == doctest::Approx(-1.0f));
        CHECK(audio.DurationSeconds() == doctest::Approx(3.0 / 48000.0));
    }

    TEST_CASE("stereo 8 bit frames are split into channels") {
        const std::vector<u8> data = {128, 192, 64, 0}; // two stereo frames
        const std::vector<u8> wav = MakeWav(1, 2, 22050, 8, data);
        auto importer = CreateWavImporter();
        ImportLog log;
        auto imported = importer->Import(Bytes(wav), serial::JsonValue::MakeObject(),
                                        AssetPath{"a/eight.wav"}, log);
        REQUIRE(imported.HasValue());
        const AudioData& audio = std::get<AudioDocument>(*imported).audio;
        CHECK(audio.channels == 2);
        CHECK(audio.frameCount == 2);
        CHECK(audio.samples.size() == 4);
        CHECK(audio.samples[0] == doctest::Approx(0.0f));   // 128
        CHECK(audio.samples[1] == doctest::Approx(0.5f));   // 192
        CHECK(audio.samples[2] == doctest::Approx(-0.5f));  // 64
        CHECK(audio.samples[3] == doctest::Approx(-1.0f));  // 0
    }

    TEST_CASE("a 24 bit wav keeps its sign") {
        std::vector<u8> data;
        // +8388607 (0x7FFFFF) and -8388608 (0x800000), little endian.
        data.push_back(0xFF); data.push_back(0xFF); data.push_back(0x7F);
        data.push_back(0x00); data.push_back(0x00); data.push_back(0x80);
        const std::vector<u8> wav = MakeWav(1, 1, 44100, 24, data);
        auto importer = CreateWavImporter();
        ImportLog log;
        auto imported = importer->Import(Bytes(wav), serial::JsonValue::MakeObject(),
                                        AssetPath{"a/deep.wav"}, log);
        REQUIRE(imported.HasValue());
        const AudioData& audio = std::get<AudioDocument>(*imported).audio;
        CHECK(audio.frameCount == 2);
        CHECK(audio.samples[0] == doctest::Approx(1.0f).epsilon(1e-6));
        CHECK(audio.samples[1] == doctest::Approx(-1.0f).epsilon(1e-6));
    }

    TEST_CASE("a float wav passes its samples through") {
        std::vector<u8> data;
        const f32 values[2] = {0.25f, -0.75f};
        const auto* raw = reinterpret_cast<const u8*>(values);
        data.insert(data.end(), raw, raw + sizeof(values));
        const std::vector<u8> wav = MakeWav(3, 1, 44100, 32, data);
        auto importer = CreateWavImporter();
        ImportLog log;
        auto imported = importer->Import(Bytes(wav), serial::JsonValue::MakeObject(),
                                        AssetPath{"a/float.wav"}, log);
        REQUIRE(imported.HasValue());
        const AudioData& audio = std::get<AudioDocument>(*imported).audio;
        CHECK(audio.samples[0] == doctest::Approx(0.25f));
        CHECK(audio.samples[1] == doctest::Approx(-0.75f));
    }

    TEST_CASE("chunks of odd length keep the walk aligned") {
        BinWriter samples;
        samples.U16(8192);   // +0.25
        samples.U16(57344);  // -0.25
        const std::vector<u8> wav = MakeWav(1, 1, 8000, 16, samples.Bytes(), true);
        auto importer = CreateWavImporter();
        ImportLog log;
        auto imported = importer->Import(Bytes(wav), serial::JsonValue::MakeObject(),
                                        AssetPath{"a/padded.wav"}, log);
        REQUIRE(imported.HasValue());
        const AudioData& audio = std::get<AudioDocument>(*imported).audio;
        CHECK(audio.frameCount == 2);
        CHECK(audio.samples[0] == doctest::Approx(0.25f));
        CHECK(audio.samples[1] == doctest::Approx(-0.25f));
    }

    TEST_CASE("compressed and malformed wavs are rejected") {
        auto importer = CreateWavImporter();
        ImportLog log;
        const AssetPath path{"a/bad.wav"};

        auto notWav = importer->Import(Bytes({1, 2, 3, 4}), serial::JsonValue::MakeObject(), path, log);
        CHECK(notWav.Error().Code() == StatusCode::ParseError);

        // ADPCM (format 2) is not decoded into noise.
        const std::vector<u8> adpcm = MakeWav(2, 1, 8000, 4, std::vector<u8>(8, 0));
        auto compressed = importer->Import(Bytes(adpcm), serial::JsonValue::MakeObject(), path, log);
        CHECK(compressed.Error().Code() == StatusCode::Unsupported);

        // A file with no data chunk.
        BinWriter samples;
        samples.U16(0);
        std::vector<u8> noData = MakeWav(1, 1, 8000, 16, samples.Bytes());
        noData.resize(noData.size() - 10); // Drop the data chunk header and payload.
        auto missing = importer->Import(Bytes(noData), serial::JsonValue::MakeObject(), path, log);
        CHECK(missing.IsError());
    }
}

TEST_SUITE("assets.database") {
    TEST_CASE("a scan finds the assets, assigns ids and writes sidecars") {
        ProjectFixture project;
        AssetDatabase database = project.MakeDatabase();
        auto report = database.Scan(project.importers);
        REQUIRE(report.HasValue());
        CHECK(report->added == 3);
        CHECK(report->removed == 0);
        CHECK(report->modified == 0);
        CHECK(report->skipped == 0);
        CHECK(report->warnings.empty());
        CHECK(database.Count() == 3);

        const AssetRecord* cube = database.FindByPath(AssetPath{"models/cube.glb"});
        REQUIRE(cube != nullptr);
        CHECK(cube->type == AssetType::Mesh);
        CHECK(cube->importer == "glb");
        CHECK(cube->importerVersion == 1);
        CHECK_FALSE(cube->id.IsNull());
        CHECK(cube->NeedsImport());

        const AssetRecord* texture = database.FindByPath(AssetPath{"textures/albedo.png"});
        REQUIRE(texture != nullptr);
        CHECK(texture->type == AssetType::Texture);
        CHECK(texture->importer == "image");

        const AssetRecord* sound = database.FindByPath(AssetPath{"audio/ping.wav"});
        REQUIRE(sound != nullptr);
        CHECK(sound->type == AssetType::AudioClip);
        CHECK(sound->importer == "wav");

        // Sidecars are what make the ids survive a rename.
        CHECK(project.source.Exists("models/cube.glb.l3dmeta"));
        CHECK(project.source.Exists("textures/albedo.png.l3dmeta"));
        CHECK(database.NeedsImportCount() == 3);
    }

    TEST_CASE("scanning an unchanged tree reports nothing") {
        ProjectFixture project;
        AssetDatabase database = project.MakeDatabase();
        REQUIRE(database.Scan(project.importers).HasValue());
        auto second = database.Scan(project.importers);
        REQUIRE(second.HasValue());
        CHECK(second->added == 0);
        CHECK(second->removed == 0);
        CHECK(second->modified == 0);
        CHECK(second->moved == 0);
        CHECK(second->Empty());
        CHECK(database.Count() == 3);
    }

    TEST_CASE("a rename without its sidecar keeps the id") {
        ProjectFixture project;
        AssetDatabase database = project.MakeDatabase();
        REQUIRE(database.Scan(project.importers).HasValue());
        const AssetRecord* before = database.FindByPath(AssetPath{"models/cube.glb"});
        REQUIRE(before != nullptr);
        const AssetId originalId = before->id;

        // Move the file and lose the sidecar, the way a careless rename does.
        REQUIRE(project.source.WriteFile("meshes/cube.glb", Bytes(project.glb)).HasValue());
        REQUIRE(project.source.Remove("models/cube.glb").HasValue());
        REQUIRE(project.source.Remove("models/cube.glb.l3dmeta").HasValue());

        auto report = database.Scan(project.importers);
        REQUIRE(report.HasValue());
        CHECK(report->added == 0);
        CHECK(report->moved == 1);
        REQUIRE(report->movedPaths.size() == 1);
        CHECK(report->movedPaths[0] == "meshes/cube.glb");
        // The old path is not reported as removed: it became the new one.
        CHECK(report->removed == 0);
        CHECK(database.Count() == 3);

        const AssetRecord* after = database.FindByPath(AssetPath{"meshes/cube.glb"});
        REQUIRE(after != nullptr);
        CHECK(after->id == originalId);
        CHECK(database.FindByPath(AssetPath{"models/cube.glb"}) == nullptr);
        // The recovered record was written a fresh sidecar at its new home.
        CHECK(project.source.Exists("meshes/cube.glb.l3dmeta"));
    }

    TEST_CASE("a rename that carries its sidecar is a move, not a deletion") {
        ProjectFixture project;
        AssetDatabase database = project.MakeDatabase();
        REQUIRE(database.Scan(project.importers).HasValue());
        const AssetId originalId = database.FindByPath(AssetPath{"models/cube.glb"})->id;

        // Move the file *and* its sidecar, the way a rename inside an editor
        // that knows about sidecars would.
        auto meta = ReadFileAsText(project.source, "models/cube.glb.l3dmeta");
        REQUIRE(meta.HasValue());
        REQUIRE(project.source
                    .WriteFile("meshes/cube.glb.l3dmeta",
                               Bytes(std::vector<u8>(meta->begin(), meta->end())))
                    .HasValue());
        REQUIRE(project.source.WriteFile("meshes/cube.glb", Bytes(project.glb)).HasValue());
        REQUIRE(project.source.Remove("models/cube.glb").HasValue());
        REQUIRE(project.source.Remove("models/cube.glb.l3dmeta").HasValue());

        auto report = database.Scan(project.importers);
        REQUIRE(report.HasValue());
        CHECK(report->moved == 1);
        CHECK(report->removed == 0);
        CHECK(report->added == 0);
        REQUIRE(report->movedPaths.size() == 1);
        CHECK(report->movedPaths[0] == "meshes/cube.glb");
        CHECK(database.FindByPath(AssetPath{"meshes/cube.glb"})->id == originalId);
    }

    TEST_CASE("two files claiming one id do not alias each other") {
        ProjectFixture project;
        AssetDatabase database = project.MakeDatabase();
        REQUIRE(database.Scan(project.importers).HasValue());
        CHECK(database.Count() == 3);

        // Copy a file together with its sidecar - the classic way a project ends
        // up with two assets answering to the same id.
        auto meta = ReadFileAsText(project.source, "models/cube.glb.l3dmeta");
        REQUIRE(meta.HasValue());
        REQUIRE(project.source.WriteFile("backup/cube.glb", Bytes(project.glb)).HasValue());
        REQUIRE(project.source
                    .WriteFile("backup/cube.glb.l3dmeta",
                               Bytes(std::vector<u8>(meta->begin(), meta->end())))
                    .HasValue());

        auto report = database.Scan(project.importers);
        REQUIRE(report.HasValue());
        CHECK(report->added == 1);
        REQUIRE(report->warnings.size() == 1);
        CHECK(report->warnings[0].find("shares its id") != std::string::npos);
        CHECK(database.Count() == 4);

        const AssetRecord* original = database.FindByPath(AssetPath{"models/cube.glb"});
        const AssetRecord* copy = database.FindByPath(AssetPath{"backup/cube.glb"});
        REQUIRE(original != nullptr);
        REQUIRE(copy != nullptr);
        CHECK(original->id != copy->id);
        // The rewritten sidecar carries the new id.
        auto rewritten = AssetMeta::Load(project.source, AssetPath{"backup/cube.glb"});
        REQUIRE(rewritten.HasValue());
        CHECK(rewritten->id == copy->id);
    }

    TEST_CASE("changing the bytes marks the asset modified") {
        ProjectFixture project;
        AssetDatabase database = project.MakeDatabase();
        REQUIRE(database.Scan(project.importers).HasValue());
        const AssetId id = database.FindByPath(AssetPath{"models/cube.glb"})->id;

        std::vector<u8> edited = project.glb;
        edited[edited.size() - 1] ^= 0xFF;
        REQUIRE(project.source.WriteFile("models/cube.glb", Bytes(edited)).HasValue());

        auto report = database.Scan(project.importers);
        REQUIRE(report.HasValue());
        CHECK(report->modified == 1);
        CHECK(report->added == 0);
        REQUIRE(report->modifiedPaths.size() == 1);
        CHECK(report->modifiedPaths[0] == "models/cube.glb");
        // The id is the sidecar's, not a new one.
        CHECK(database.FindByPath(AssetPath{"models/cube.glb"})->id == id);
    }

    TEST_CASE("deleting a source file removes its record") {
        ProjectFixture project;
        AssetDatabase database = project.MakeDatabase();
        REQUIRE(database.Scan(project.importers).HasValue());
        REQUIRE(project.source.Remove("audio/ping.wav").HasValue());
        REQUIRE(project.source.Remove("audio/ping.wav.l3dmeta").HasValue());

        auto report = database.Scan(project.importers);
        REQUIRE(report.HasValue());
        CHECK(report->removed == 1);
        REQUIRE(report->removedPaths.size() == 1);
        CHECK(report->removedPaths[0] == "audio/ping.wav");
        CHECK(database.Count() == 2);
        CHECK(database.FindByPath(AssetPath{"audio/ping.wav"}) == nullptr);
    }

    TEST_CASE("files with no importer are skipped, not errors") {
        ProjectFixture project;
        REQUIRE(project.source.WriteFile("notes/readme.txt", Bytes({1, 2, 3})).HasValue());
        REQUIRE(project.source.WriteFile("models/unknown.fbx2", Bytes({1, 2, 3})).HasValue());
        AssetDatabase database = project.MakeDatabase();
        auto report = database.Scan(project.importers);
        REQUIRE(report.HasValue());
        CHECK(report->added == 3);
        CHECK(report->skipped == 2);
        CHECK(database.Count() == 3);
    }

    TEST_CASE("paths that differ only in case are reported") {
        ProjectFixture project;
        REQUIRE(project.source.WriteFile("models/Cube.glb", Bytes(project.glb)).HasValue());
        AssetDatabase database = project.MakeDatabase();
        auto report = database.Scan(project.importers);
        REQUIRE(report.HasValue());
        CHECK(report->added == 3);
        REQUIRE(report->warnings.size() == 1);
        CHECK(report->warnings[0].find("differ only in case") != std::string::npos);
    }

    TEST_CASE("cooking an asset clears its pending flag and rewrites the sidecar") {
        ProjectFixture project;
        AssetDatabase database = project.MakeDatabase();
        REQUIRE(database.Scan(project.importers).HasValue());
        const AssetRecord* cube = database.FindByPath(AssetPath{"models/cube.glb"});
        REQUIRE(cube != nullptr);
        CHECK(cube->NeedsImport());
        const AssetId id = cube->id;
        const u64 sourceHash = cube->sourceHash;
        const u64 fingerprint = cube->ImportFingerprint();

        REQUIRE(database.MarkCooked(id, sourceHash, fingerprint).HasValue());
        CHECK_FALSE(database.FindById(id)->NeedsImport());
        CHECK(database.NeedsImportCount() == 2);

        // The sidecar carries the cook state, so a fresh database agrees.
        AssetDatabase reopened;
        REQUIRE(reopened.Initialize(project.source, AssetDatabaseDesc{}).HasValue());
        REQUIRE(reopened.Scan(project.importers).HasValue());
        const AssetRecord* reloaded = reopened.FindById(id);
        REQUIRE(reloaded != nullptr);
        CHECK_FALSE(reloaded->NeedsImport());
        CHECK(reopened.NeedsImportCount() == 2);
    }

    TEST_CASE("the index cache round trips") {
        ProjectFixture project;
        AssetDatabase database = project.MakeDatabase();
        REQUIRE(database.Scan(project.importers).HasValue());
        REQUIRE(database.MarkCooked(database.FindByPath(AssetPath{"models/cube.glb"})->id,
                                    database.FindByPath(AssetPath{"models/cube.glb"})->sourceHash,
                                    database.FindByPath(AssetPath{"models/cube.glb"})->ImportFingerprint())
                    .HasValue());
        REQUIRE(database.SaveIndex().HasValue());
        CHECK(project.source.Exists(".l3dindex.json"));

        AssetDatabase cached;
        REQUIRE(cached.Initialize(project.source, AssetDatabaseDesc{}).HasValue());
        auto loaded = cached.LoadIndex();
        REQUIRE(loaded.HasValue());
        CHECK(*loaded == 3);
        const AssetRecord* cube = cached.FindByPath(AssetPath{"models/cube.glb"});
        REQUIRE(cube != nullptr);
        CHECK(cube->id == database.FindByPath(AssetPath{"models/cube.glb"})->id);
        CHECK_FALSE(cube->NeedsImport());
    }

    TEST_CASE("scanning before initialising is an error") {
        AssetDatabase database;
        auto report = database.Scan(CreateDefaultImporters());
        CHECK(report.IsError());
        CHECK(report.Error().Code() == StatusCode::NotInitialized);
    }

    TEST_CASE("a read only database does not write sidecars") {
        MemoryFileSystem fs;
        const std::vector<u8> glb = MakeTriangleGlb();
        REQUIRE(fs.WriteFile("models/cube.glb", Bytes(glb)).HasValue());
        AssetDatabase database;
        AssetDatabaseDesc desc;
        desc.writeSidecars = false;
        REQUIRE(database.Initialize(fs, desc).HasValue());
        REQUIRE(database.Scan(CreateDefaultImporters()).HasValue());
        CHECK(database.Count() == 1);
        CHECK_FALSE(fs.Exists("models/cube.glb.l3dmeta"));
    }
}

TEST_SUITE("assets.cook") {
    TEST_CASE("cooking writes engine files and a manifest") {
        ProjectFixture project;
        AssetDatabase database = project.MakeDatabase();
        REQUIRE(database.Scan(project.importers).HasValue());

        Cooker cooker;
        REQUIRE(cooker.Initialize(project.source, project.cooked, database, project.importers)
                    .HasValue());
        auto report = cooker.CookAll();
        REQUIRE(report.HasValue());
        CHECK(report->sourceFilesCooked == 3);
        CHECK(report->failed == 0);
        // Mesh + material from the GLB, plus the texture and the sound.
        CHECK(report->filesWritten == 4);
        CHECK(report->errors.empty());
        CHECK(cooker.Manifest().entries.size() == 4);
        CHECK(project.cooked.Exists("manifest.json"));

        const AssetRecord* cube = database.FindByPath(AssetPath{"models/cube.glb"});
        REQUIRE(cube != nullptr);
        const CookedManifestEntry* entry = cooker.Manifest().Find(cube->id);
        REQUIRE(entry != nullptr);
        CHECK(entry->type == AssetType::Mesh);
        CHECK(entry->name == "triangle");
        CHECK(entry->sourcePath == AssetPath{"models/cube.glb"});
        CHECK(entry->cookedPath.Extension() == ".l3dmesh");
        CHECK(project.cooked.Exists(entry->cookedPath.Text()));
        CHECK_FALSE(cube->NeedsImport());
    }

    TEST_CASE("a second cook skips everything") {
        ProjectFixture project;
        AssetDatabase database = project.MakeDatabase();
        REQUIRE(database.Scan(project.importers).HasValue());
        Cooker cooker;
        REQUIRE(cooker.Initialize(project.source, project.cooked, database, project.importers)
                    .HasValue());
        REQUIRE(cooker.CookAll().HasValue());

        auto second = cooker.CookAll();
        REQUIRE(second.HasValue());
        CHECK(second->sourceFilesCooked == 0);
        CHECK(second->filesWritten == 0);
        CHECK(second->skipped == 3);
        CHECK(second->failed == 0);
        CHECK(cooker.Manifest().entries.size() == 4);
    }

    TEST_CASE("editing one source recooks only that source") {
        ProjectFixture project;
        AssetDatabase database = project.MakeDatabase();
        REQUIRE(database.Scan(project.importers).HasValue());
        Cooker cooker;
        REQUIRE(cooker.Initialize(project.source, project.cooked, database, project.importers)
                    .HasValue());
        REQUIRE(cooker.CookAll().HasValue());

        std::vector<u8> edited = project.wav;
        edited[edited.size() - 1] ^= 0xFF;
        REQUIRE(project.source.WriteFile("audio/ping.wav", Bytes(edited)).HasValue());
        REQUIRE(database.Scan(project.importers).HasValue());

        auto report = cooker.CookAll();
        REQUIRE(report.HasValue());
        CHECK(report->sourceFilesCooked == 1);
        CHECK(report->filesWritten == 1);
        CHECK(report->skipped == 2);
        REQUIRE(report->cookedPaths.size() == 1);
        CHECK(report->cookedPaths[0] == "audio/ping.wav");
    }

    TEST_CASE("deleting a source removes its cooked file and manifest entry") {
        ProjectFixture project;
        AssetDatabase database = project.MakeDatabase();
        REQUIRE(database.Scan(project.importers).HasValue());
        Cooker cooker;
        REQUIRE(cooker.Initialize(project.source, project.cooked, database, project.importers)
                    .HasValue());
        REQUIRE(cooker.CookAll().HasValue());
        const AssetId soundId = database.FindByPath(AssetPath{"audio/ping.wav"})->id;
        const CookedManifestEntry* entry = cooker.Manifest().Find(soundId);
        REQUIRE(entry != nullptr);
        const std::string cookedPath = entry->cookedPath.ToString();
        CHECK(project.cooked.Exists(cookedPath));

        REQUIRE(project.source.Remove("audio/ping.wav").HasValue());
        REQUIRE(project.source.Remove("audio/ping.wav.l3dmeta").HasValue());
        REQUIRE(database.Scan(project.importers).HasValue());
        REQUIRE(cooker.CookAll().HasValue());

        // Mesh + material + texture are left; the sound is gone.
        CHECK(cooker.Manifest().entries.size() == 3);
        CHECK(cooker.Manifest().Find(soundId) == nullptr);
        CHECK_FALSE(project.cooked.Exists(cookedPath));
    }

    TEST_CASE("a glb cooks its mesh, its material and its embedded texture") {
        const std::vector<u8> png = MakePng(2, 2, SolidRgba(2, 2, 0, 0, 255, 255));
        MemoryFileSystem source;
        MemoryFileSystem cooked;
        REQUIRE(source.WriteFile("models/textured.glb", Bytes(MakeEmbeddedTextureGlb(png)))
                    .HasValue());

        ImporterRegistry importers = CreateDefaultImporters();
        AssetDatabase database;
        REQUIRE(database.Initialize(source, AssetDatabaseDesc{}).HasValue());
        REQUIRE(database.Scan(importers).HasValue());

        Cooker cooker;
        REQUIRE(cooker.Initialize(source, cooked, database, importers).HasValue());
        auto report = cooker.CookAll();
        REQUIRE(report.HasValue());
        CHECK(report->failed == 0);
        REQUIRE(report->errors.empty());
        // One mesh + one material + one embedded texture.
        CHECK(cooker.Manifest().entries.size() == 3);

        const AssetRecord* record = database.FindByPath(AssetPath{"models/textured.glb"});
        REQUIRE(record != nullptr);

        AssetManager assets;
        REQUIRE(assets.Initialize(cooked).HasValue());
        auto mesh = assets.GetMesh(record->id);
        REQUIRE(mesh.HasValue());
        CHECK((*mesh)->meshes.size() == 1);
        CHECK((*mesh)->meshes[0].name == "quad");

        const CookedManifestEntry* materialEntry = nullptr;
        for (const CookedManifestEntry& entry : assets.Manifest().entries) {
            if (entry.type == AssetType::Material) {
                materialEntry = &entry;
            }
        }
        REQUIRE(materialEntry != nullptr);
        auto material = assets.GetMaterial(materialEntry->id);
        REQUIRE(material.HasValue());
        CHECK((*material)->data.name == "textured");
        CHECK((*material)->data.roughness == doctest::Approx(0.9f));
        // The base colour slot points at a real cooked texture asset.
        const AssetId textureId = (*material)->baseColorTexture;
        CHECK_FALSE(textureId.IsNull());
        CHECK((*material)->normalTexture.IsNull());
        REQUIRE(materialEntry->dependencies.size() == 1);
        CHECK(materialEntry->dependencies[0] == textureId);

        auto texture = assets.GetTexture(textureId);
        REQUIRE(texture.HasValue());
        CHECK((*texture)->image.width == 2);
        CHECK((*texture)->image.height == 2);
        // Sub-asset ids are deterministic, so cooked file names are stable.
        CHECK(textureId == record->id.Sub(2));
    }

    TEST_CASE("cooked meshes survive the binary round trip exactly") {
        ProjectFixture project;
        AssetDatabase database = project.MakeDatabase();
        REQUIRE(database.Scan(project.importers).HasValue());
        Cooker cooker;
        REQUIRE(cooker.Initialize(project.source, project.cooked, database, project.importers)
                    .HasValue());
        REQUIRE(cooker.CookAll().HasValue());

        AssetManager assets;
        REQUIRE(assets.Initialize(project.cooked).HasValue());
        const AssetId cubeId = database.FindByPath(AssetPath{"models/cube.glb"})->id;
        auto document = assets.GetMesh(cubeId);
        REQUIRE(document.HasValue());
        REQUIRE((*document)->meshes.size() == 1);
        const MeshData& mesh = (*document)->meshes[0];
        CHECK(mesh.name == "triangle");
        REQUIRE(mesh.positions.size() == 3);
        CHECK(mesh.positions[1].x == doctest::Approx(1.0f));
        CHECK(mesh.positions[2].y == doctest::Approx(2.0f));
        CHECK(mesh.normals[0].z == doctest::Approx(1.0f));
        CHECK(mesh.uvs[2].y == doctest::Approx(1.0f));
        CHECK(mesh.indices[1] == 1);
        CHECK(mesh.bounds.max.y == doctest::Approx(2.0f));
        CHECK(mesh.materialIndex == 0);

        // Audio and textures come back intact too.
        auto sound = assets.GetAudio(database.FindByPath(AssetPath{"audio/ping.wav"})->id);
        REQUIRE(sound.HasValue());
        CHECK((*sound)->audio.frameCount == 4);
        CHECK((*sound)->audio.samples[0] == doctest::Approx(0.5f));
        CHECK((*sound)->audio.samples[1] == doctest::Approx(-0.5f));
        CHECK((*sound)->audio.samples[3] == doctest::Approx(-1.0f));

        auto texture = assets.GetTexture(database.FindByPath(AssetPath{"textures/albedo.png"})->id);
        REQUIRE(texture.HasValue());
        CHECK((*texture)->image.width == 4);
        CHECK((*texture)->image.mipLevels == 3);
        CHECK((*texture)->image.format == rhi::Format::RGBA8_SRGB);
    }

    TEST_CASE("the cooked formats reject foreign and truncated data") {
        ProjectFixture project;
        AssetDatabase database = project.MakeDatabase();
        REQUIRE(database.Scan(project.importers).HasValue());
        Cooker cooker;
        REQUIRE(cooker.Initialize(project.source, project.cooked, database, project.importers)
                    .HasValue());
        REQUIRE(cooker.CookAll().HasValue());
        const AssetId cubeId = database.FindByPath(AssetPath{"models/cube.glb"})->id;

        auto raw = [&]() {
            AssetManager probe;
            REQUIRE(probe.Initialize(project.cooked).HasValue());
            auto bytes = probe.LoadRaw(cubeId);
            REQUIRE(bytes.HasValue());
            return *bytes;
        }();

        // Reading mesh bytes as a texture must fail on the tag.
        auto asTexture = ReadTextureDocument(Bytes(raw));
        CHECK(asTexture.IsError());
        CHECK(asTexture.Error().Code() == StatusCode::ParseError);

        std::vector<u8> truncated = raw;
        truncated.resize(20);
        auto shortMesh = ReadMeshDocument(Bytes(truncated));
        CHECK(shortMesh.IsError());

        std::vector<u8> wrongVersion = raw;
        wrongVersion[4] = 99;
        auto versioned = ReadMeshDocument(Bytes(wrongVersion));
        CHECK(versioned.Error().Code() == StatusCode::Unsupported);

        CHECK(ReadMeshDocument(Bytes({})).IsError());
        CHECK(ReadMeshDocument(Bytes({1, 2, 3})).IsError());
    }

    TEST_CASE("the manifest round trips through json") {
        CookedManifest manifest;
        CookedManifestEntry entry;
        entry.id = AssetId::FromName("models/cube");
        entry.sourcePath = AssetPath{"models/cube.glb"};
        entry.cookedPath = AssetPath{"abc.l3dmesh"};
        entry.type = AssetType::Mesh;
        entry.name = "triangle";
        entry.dependencies.push_back(AssetId::FromName("textures/albedo"));
        manifest.entries.push_back(entry);

        auto parsed = CookedManifest::Parse(manifest.Dump());
        REQUIRE(parsed.HasValue());
        REQUIRE(parsed->entries.size() == 1);
        const CookedManifestEntry& back = parsed->entries[0];
        CHECK(back.id == entry.id);
        CHECK(back.sourcePath == entry.sourcePath);
        CHECK(back.cookedPath == entry.cookedPath);
        CHECK(back.type == AssetType::Mesh);
        CHECK(back.name == "triangle");
        REQUIRE(back.dependencies.size() == 1);
        CHECK(back.dependencies[0] == entry.dependencies[0]);
        CHECK(parsed->Find(entry.id) != nullptr);
        CHECK(CookedManifest::Parse("{}").IsError());
        CHECK(CookedManifest::Parse("nonsense").IsError());
    }

    TEST_CASE("cooked file names follow the asset type") {
        const AssetId id = AssetId::FromName("x");
        CHECK(CookedFileName(id, AssetType::Mesh).find(".l3dmesh") != std::string::npos);
        CHECK(CookedFileName(id, AssetType::Texture).find(".l3dtex") != std::string::npos);
        CHECK(CookedFileName(id, AssetType::AudioClip).find(".l3daudio") != std::string::npos);
        CHECK(CookedFileName(id, AssetType::Material).find(".l3dmat") != std::string::npos);
        CHECK(CookedFileName(id, AssetType::Mesh).size() == 36 + 8);
    }
}

TEST_SUITE("assets.runtime") {
    TEST_CASE("a runtime with no cooked data fails at start up") {
        MemoryFileSystem empty;
        AssetManager assets;
        auto initialized = assets.Initialize(empty);
        CHECK(initialized.IsError());
        CHECK(initialized.Error().Code() == StatusCode::NotFound);
        CHECK_FALSE(assets.IsInitialized());
        CHECK(assets.GetMesh(AssetId::FromName("anything")).IsError());
    }

    TEST_CASE("assets are cached, released and typed") {
        ProjectFixture project;
        AssetDatabase database = project.MakeDatabase();
        REQUIRE(database.Scan(project.importers).HasValue());
        Cooker cooker;
        REQUIRE(cooker.Initialize(project.source, project.cooked, database, project.importers)
                    .HasValue());
        REQUIRE(cooker.CookAll().HasValue());

        AssetManager assets;
        REQUIRE(assets.Initialize(project.cooked).HasValue());
        const AssetId cubeId = database.FindByPath(AssetPath{"models/cube.glb"})->id;
        const AssetId pngId = database.FindByPath(AssetPath{"textures/albedo.png"})->id;

        CHECK(assets.CachedCount() == 0);
        auto first = assets.GetMesh(cubeId);
        REQUIRE(first.HasValue());
        CHECK(assets.CachedCount() == 1);
        CHECK(assets.CachedCookedBytes() > 0);

        // Loading something else must not invalidate a pointer already handed
        // out - that is the classic cache crash.
        auto texture = assets.GetTexture(pngId);
        REQUIRE(texture.HasValue());
        auto second = assets.GetMesh(cubeId);
        REQUIRE(second.HasValue());
        CHECK(*first == *second);
        CHECK((*first)->meshes[0].name == "triangle");

        // Wrong type for the id.
        auto wrong = assets.GetMesh(pngId);
        CHECK(wrong.IsError());
        CHECK(wrong.Error().Code() == StatusCode::InvalidArgument);

        auto missing = assets.GetMesh(AssetId::FromName("not-there"));
        CHECK(missing.Error().Code() == StatusCode::NotFound);

        assets.Release(cubeId);
        CHECK(assets.CachedCount() == 1);
        auto reloaded = assets.GetMesh(cubeId);
        REQUIRE(reloaded.HasValue());
        CHECK(assets.CachedCount() == 2);

        assets.ClearCache();
        CHECK(assets.CachedCount() == 0);
        CHECK(assets.CachedCookedBytes() == 0);
    }

    TEST_CASE("the manifest can be searched by source path") {
        ProjectFixture project;
        AssetDatabase database = project.MakeDatabase();
        REQUIRE(database.Scan(project.importers).HasValue());
        Cooker cooker;
        REQUIRE(cooker.Initialize(project.source, project.cooked, database, project.importers)
                    .HasValue());
        REQUIRE(cooker.CookAll().HasValue());

        AssetManager assets;
        REQUIRE(assets.Initialize(project.cooked).HasValue());
        const CookedManifestEntry* entry = assets.FindBySourcePath(AssetPath{"textures/albedo.png"});
        REQUIRE(entry != nullptr);
        CHECK(entry->type == AssetType::Texture);
        CHECK(assets.FindBySourcePath(AssetPath{"nope.png"}) == nullptr);
    }
}

TEST_SUITE("assets.gpu") {
    TEST_CASE("cooked image data uploads into a gpu texture") {
        const std::vector<u8> png = MakePng(4, 4, SolidRgba(4, 4, 12, 34, 56, 255));
        auto importer = CreateImageImporter();
        ImportLog log;
        TextureImportSettings settings;
        settings.srgb = false;
        auto imported = importer->Import(Bytes(png), TextureSettingsToJson(settings),
                                        AssetPath{"t/a.png"}, log);
        REQUIRE(imported.HasValue());
        const ImageData& image = std::get<TextureDocument>(*imported).image;

        rhi::DeviceDesc desc;
        desc.preferredBackend = rhi::BackendType::Null;
        desc.enableValidation = true;
        auto device = rhi::CreateDevice(desc);
        REQUIRE(device.HasValue());

        auto texture = UploadTexture(**device, image, "albedo");
        REQUIRE(texture.HasValue());
        CHECK((*texture)->Width() == 4);
        CHECK((*texture)->Height() == 4);
        CHECK((*texture)->Desc().mipLevels == 3);
        CHECK((*texture)->Desc().format == rhi::Format::RGBA8_UNorm);
        CHECK((*texture)->Desc().debugName == "albedo");
        CHECK((*device)->ValidationErrorCount() == 0);
    }

    TEST_CASE("an incomplete image is refused before touching the device") {
        rhi::DeviceDesc desc;
        desc.preferredBackend = rhi::BackendType::Null;
        desc.enableValidation = true;
        auto device = rhi::CreateDevice(desc);
        REQUIRE(device.HasValue());

        ImageData empty;
        auto uploaded = UploadTexture(**device, empty);
        CHECK(uploaded.IsError());
        CHECK(uploaded.Error().Code() == StatusCode::InvalidArgument);

        ImageData mismatched;
        mismatched.width = 2;
        mismatched.height = 2;
        mismatched.mipLevels = 2;
        mismatched.pixels = {1, 2, 3, 4}; // One texel, not four.
        auto bad = UploadTexture(**device, mismatched);
        CHECK(bad.IsError());
    }

    TEST_CASE("pipeline geometry feeds the renderer") {
        ProjectFixture project;
        AssetDatabase database = project.MakeDatabase();
        REQUIRE(database.Scan(project.importers).HasValue());
        Cooker cooker;
        REQUIRE(cooker.Initialize(project.source, project.cooked, database, project.importers)
                    .HasValue());
        REQUIRE(cooker.CookAll().HasValue());

        AssetManager assets;
        REQUIRE(assets.Initialize(project.cooked).HasValue());
        const AssetId cubeId = database.FindByPath(AssetPath{"models/cube.glb"})->id;
        auto document = assets.GetMesh(cubeId);
        REQUIRE(document.HasValue());

        rhi::DeviceDesc desc;
        desc.preferredBackend = rhi::BackendType::Null;
        desc.enableValidation = true;
        auto device = rhi::CreateDevice(desc);
        REQUIRE(device.HasValue());

        render::Renderer renderer(render::RendererSettings{});
        auto handle = renderer.RegisterMesh(**device, (*document)->meshes[0]);
        REQUIRE(handle.HasValue());
        CHECK(*handle != render::kInvalidMesh);
        CHECK((*device)->ValidationErrorCount() == 0);
        // The renderer's own statistics agree that the geometry arrived.
        auto uploaded = renderer.RegisterMesh(**device, (*document)->meshes[0]);
        REQUIRE(uploaded.HasValue());
        CHECK(*uploaded == *handle + 1);
    }
}

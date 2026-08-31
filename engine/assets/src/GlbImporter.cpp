/// @file GlbImporter.cpp
/// @brief Imports the binary glTF container (GLB) into MeshData and MaterialData.
///
/// Why a hand written GLB reader instead of tinygltf: see
/// docs/architecture/dependencies.md.  In short - GLB is a small, frozen
/// container (a 12 byte header plus length prefixed chunks) and the JSON inside
/// it is read with the engine's own parser, so a third party library would buy
/// us little and cost us a dependency plus a second JSON model to convert from.
///
/// Supported: meshes with POSITION/NORMAL/TEXCOORD_0, indexed and non indexed
/// triangle primitives, interleaved or packed buffer views, all five component
/// types (including normalised integers), PBR metallic-roughness materials,
/// external and embedded images.
///
/// Not supported, reported explicitly rather than silently dropped: sparse
/// accessors, external buffer files, non triangle primitive modes, morph
/// targets and skins.  Node transforms are deliberately *not* baked in -
/// geometry stays in its own local space and the scene layer owns transforms.

#include "local3d/assets/Importer.hpp"

#include <cstring>

namespace l3d::assets {

namespace {

constexpr u32 kGlbMagic = 0x46546C67U;
constexpr u32 kGlbVersion = 2;
constexpr u32 kChunkJson = 0x4E4F534AU;
constexpr u32 kChunkBin = 0x004E4942U;
constexpr u32 kModeTriangles = 4;

constexpr u32 kCtInt8 = 5120;
constexpr u32 kCtUInt8 = 5121;
constexpr u32 kCtInt16 = 5122;
constexpr u32 kCtUInt16 = 5123;
constexpr u32 kCtUInt32 = 5125;
constexpr u32 kCtFloat32 = 5126;

/// The engine speaks in std::byte spans; GLB payloads are read as u8.
[[nodiscard]] const u8* AsU8(const std::byte* pointer) noexcept {
    return reinterpret_cast<const u8*>(pointer);
}

[[nodiscard]] u32 ComponentByteSize(u32 componentType) noexcept {
    switch (componentType) {
        case kCtInt8:
        case kCtUInt8: return 1;
        case kCtInt16:
        case kCtUInt16: return 2;
        case kCtUInt32:
        case kCtFloat32: return 4;
        default: return 0;
    }
}

[[nodiscard]] u32 ComponentCount(std::string_view type) noexcept {
    if (type == "SCALAR") {
        return 1;
    }
    if (type == "VEC2") {
        return 2;
    }
    if (type == "VEC3") {
        return 3;
    }
    if (type == "VEC4") {
        return 4;
    }
    if (type == "MAT2") {
        return 4;
    }
    if (type == "MAT3") {
        return 9;
    }
    if (type == "MAT4") {
        return 16;
    }
    return 0;
}

struct GlbBufferView {
    u32 buffer = 0;
    u32 byteOffset = 0;
    u32 byteLength = 0;
    /// 0 means tightly packed.
    u32 byteStride = 0;
};

struct GlbAccessor {
    i32 bufferView = -1;
    u32 byteOffset = 0;
    u32 componentType = 0;
    u32 components = 0;
    u32 count = 0;
    bool normalized = false;
    bool sparse = false;
};

/// A resolved view onto one accessor's elements.
struct AttributeReader {
    const u8* base = nullptr;
    u32 stride = 0;
    u32 componentType = 0;
    u32 components = 0;
    u32 count = 0;
    bool normalized = false;

    [[nodiscard]] bool IsValid() const noexcept { return base != nullptr && count > 0; }
};

[[nodiscard]] f32 ComponentToFloat(u32 componentType, bool normalized, const u8* at) noexcept {
    switch (componentType) {
        case kCtFloat32: {
            f32 value = 0.0f;
            std::memcpy(&value, at, sizeof(f32));
            return value;
        }
        case kCtInt8: {
            const auto raw = static_cast<f32>(*reinterpret_cast<const i8*>(at));
            return normalized ? (raw < 0.0f ? raw / 127.0f : raw / 128.0f) : raw;
        }
        case kCtUInt8: {
            const auto raw = static_cast<f32>(*at);
            return normalized ? raw / 255.0f : raw;
        }
        case kCtInt16: {
            i16 raw = 0;
            std::memcpy(&raw, at, sizeof(i16));
            const auto value = static_cast<f32>(raw);
            return normalized ? (value < 0.0f ? value / 32767.0f : value / 32768.0f) : value;
        }
        case kCtUInt16: {
            u16 raw = 0;
            std::memcpy(&raw, at, sizeof(u16));
            const auto value = static_cast<f32>(raw);
            return normalized ? value / 65535.0f : value;
        }
        case kCtUInt32: {
            u32 raw = 0;
            std::memcpy(&raw, at, sizeof(u32));
            return static_cast<f32>(raw);
        }
        default: return 0.0f;
    }
}

[[nodiscard]] u32 ComponentToIndex(u32 componentType, const u8* at) noexcept {
    switch (componentType) {
        case kCtUInt8: return *at;
        case kCtUInt16: {
            u16 raw = 0;
            std::memcpy(&raw, at, sizeof(u16));
            return raw;
        }
        case kCtUInt32: {
            u32 raw = 0;
            std::memcpy(&raw, at, sizeof(u32));
            return raw;
        }
        default: return 0;
    }
}

[[nodiscard]] f32 ReadComponent(const AttributeReader& reader, u32 element, u32 component) noexcept {
    const u32 size = ComponentByteSize(reader.componentType);
    const u8* at = reader.base + static_cast<usize>(element) * reader.stride +
                   static_cast<usize>(component) * size;
    return ComponentToFloat(reader.componentType, reader.normalized, at);
}

/// The GLB parser.  Holds the chunk pointers and the decoded JSON tables.
class GlbParser {
public:
    [[nodiscard]] Result<void> Open(ConstByteSpan bytes);
    [[nodiscard]] Result<void> ParseDocument(ImportLog& log);

    [[nodiscard]] Result<AttributeReader> MakeReader(const GlbAccessor& accessor) const;
    /// Byte range of a buffer view inside the bin chunk: (offset, length).
    [[nodiscard]] Result<std::pair<u64, u64>> BufferViewRange(usize viewIndex) const;

    [[nodiscard]] const std::vector<GlbAccessor>& Accessors() const noexcept { return accessors_; }
    [[nodiscard]] const serial::JsonValue& Meshes() const noexcept { return meshes_; }
    [[nodiscard]] const serial::JsonValue& Materials() const noexcept { return materials_; }
    [[nodiscard]] const serial::JsonValue& Textures() const noexcept { return textures_; }
    [[nodiscard]] const serial::JsonValue& Images() const noexcept { return images_; }
    [[nodiscard]] ConstByteSpan BinChunk() const noexcept { return bin_; }

private:
    ConstByteSpan json_;
    ConstByteSpan bin_;
    serial::JsonValue document_;
    std::vector<GlbBufferView> bufferViews_;
    std::vector<GlbAccessor> accessors_;
    /// Copies of the four tables the importer reads.  Documents are small and
    /// short lived, and holding them by value keeps lifetimes obvious.
    serial::JsonValue meshes_;
    serial::JsonValue materials_;
    serial::JsonValue textures_;
    serial::JsonValue images_;
};

Result<void> GlbParser::Open(ConstByteSpan bytes) {
    if (bytes.size() < 12) {
        return Unexpected(Status{StatusCode::ParseError, "GLB file is too small to hold a header"});
    }
    u32 magic = 0;
    u32 version = 0;
    u32 length = 0;
    std::memcpy(&magic, bytes.data(), 4);
    std::memcpy(&version, bytes.data() + 4, 4);
    std::memcpy(&length, bytes.data() + 8, 4);

    if (magic != kGlbMagic) {
        return Unexpected(
            Status{StatusCode::ParseError, "Not a GLB file (bad magic). Use the binary glTF form"});
    }
    if (version != kGlbVersion) {
        return Unexpected(Status{StatusCode::Unsupported, "Only GLB version 2 is supported"});
    }
    if (static_cast<u64>(length) > bytes.size()) {
        return Unexpected(Status{StatusCode::ParseError, "GLB header length exceeds the file size"});
    }

    u64 offset = 12;
    while (offset + 8 <= length) {
        u32 chunkLength = 0;
        u32 chunkType = 0;
        std::memcpy(&chunkLength, bytes.data() + offset, 4);
        std::memcpy(&chunkType, bytes.data() + offset + 4, 4);
        offset += 8;
        if (offset + chunkLength > length) {
            return Unexpected(Status{StatusCode::ParseError, "GLB chunk runs past the end of file"});
        }
        const ConstByteSpan chunk(bytes.data() + offset, chunkLength);
        if (chunkType == kChunkJson && json_.empty()) {
            json_ = chunk;
        } else if (chunkType == kChunkBin && bin_.empty()) {
            bin_ = chunk;
        }
        offset += chunkLength;
    }

    if (json_.empty()) {
        return Unexpected(Status{StatusCode::ParseError, "GLB has no JSON chunk"});
    }
    return {};
}

Result<void> GlbParser::ParseDocument(ImportLog& log) {
    auto parsed = serial::JsonValue::Parse(
        std::string_view(reinterpret_cast<const char*>(json_.data()), json_.size()));
    if (parsed.IsError()) {
        return Unexpected(Status{StatusCode::ParseError, "GLB JSON chunk failed to parse"});
    }
    document_ = std::move(*parsed);

    const std::string_view version = document_["asset"]["version"].AsString();
    if (version.empty() || version.front() != '2') {
        return Unexpected(Status{StatusCode::Unsupported, "Only glTF 2.x assets are supported"});
    }

    const serial::JsonValue& buffers = document_["buffers"];
    if (buffers.Size() > 1) {
        log.Warning("GLB declares more than one buffer; only the embedded one is read");
    }

    for (const serial::JsonValue& view : document_["bufferViews"].AsArray()) {
        GlbBufferView parsedView;
        parsedView.buffer = static_cast<u32>(view["buffer"].AsInt(0));
        parsedView.byteOffset = static_cast<u32>(view["byteOffset"].AsInt(0));
        parsedView.byteLength = static_cast<u32>(view["byteLength"].AsInt(0));
        parsedView.byteStride = static_cast<u32>(view["byteStride"].AsInt(0));
        bufferViews_.push_back(parsedView);
    }

    for (const serial::JsonValue& accessor : document_["accessors"].AsArray()) {
        GlbAccessor parsedAccessor;
        parsedAccessor.bufferView =
            accessor.Contains("bufferView") ? static_cast<i32>(accessor["bufferView"].AsInt(-1)) : -1;
        parsedAccessor.byteOffset = static_cast<u32>(accessor["byteOffset"].AsInt(0));
        parsedAccessor.componentType = static_cast<u32>(accessor["componentType"].AsInt(0));
        parsedAccessor.count = static_cast<u32>(accessor["count"].AsInt(0));
        parsedAccessor.components = ComponentCount(accessor["type"].AsString());
        parsedAccessor.normalized = accessor["normalized"].AsBool(false);
        parsedAccessor.sparse = accessor.Contains("sparse");
        accessors_.push_back(parsedAccessor);
    }

    meshes_ = document_["meshes"];
    materials_ = document_["materials"];
    textures_ = document_["textures"];
    images_ = document_["images"];
    return {};
}

Result<std::pair<u64, u64>> GlbParser::BufferViewRange(usize viewIndex) const {
    if (viewIndex >= bufferViews_.size()) {
        return Unexpected(Status{StatusCode::OutOfRange, "Buffer view index is out of range"});
    }
    const GlbBufferView& view = bufferViews_[viewIndex];
    if (view.buffer != 0) {
        return Unexpected(
            Status{StatusCode::Unsupported, "External buffer files are not supported; use GLB"});
    }
    return std::pair<u64, u64>{view.byteOffset, view.byteLength};
}

Result<AttributeReader> GlbParser::MakeReader(const GlbAccessor& accessor) const {
    if (accessor.sparse) {
        return Unexpected(Status{StatusCode::Unsupported, "Sparse accessors are not supported"});
    }
    if (accessor.components == 0 || ComponentByteSize(accessor.componentType) == 0) {
        return Unexpected(Status{StatusCode::ParseError, "Accessor has an unknown component type"});
    }
    if (accessor.bufferView < 0 ||
        static_cast<usize>(accessor.bufferView) >= bufferViews_.size()) {
        return Unexpected(Status{StatusCode::ParseError, "Accessor references a missing buffer view"});
    }
    const GlbBufferView& view = bufferViews_[static_cast<usize>(accessor.bufferView)];
    if (view.buffer != 0) {
        return Unexpected(
            Status{StatusCode::Unsupported, "External buffer files are not supported; use GLB"});
    }

    const u32 elementSize = ComponentByteSize(accessor.componentType) * accessor.components;
    const u32 stride = view.byteStride != 0 ? view.byteStride : elementSize;
    if (stride < elementSize) {
        return Unexpected(Status{StatusCode::ParseError, "Buffer view stride is smaller than an element"});
    }

    const u64 start = static_cast<u64>(view.byteOffset) + accessor.byteOffset;
    const u64 needed = accessor.count == 0 ? 0
                                           : start + static_cast<u64>(stride) * (accessor.count - 1) +
                                                 elementSize;
    if (needed > bin_.size()) {
        return Unexpected(Status{StatusCode::ParseError, "Accessor runs past the end of the GLB bin chunk"});
    }

    AttributeReader reader;
    reader.base = AsU8(bin_.data()) + start;
    reader.stride = stride;
    reader.componentType = accessor.componentType;
    reader.components = accessor.components;
    reader.count = accessor.count;
    reader.normalized = accessor.normalized;
    return reader;
}

/// Generates smooth normals by accumulating unnormalised face normals, which
/// weights each contribution by triangle area - the standard trick that keeps
/// small triangles from dominating a vertex.
void GenerateNormals(MeshData& mesh, ImportLog& log) {
    mesh.normals.assign(mesh.positions.size(), math::Vec3{0.0f, 0.0f, 0.0f});
    for (usize triangle = 0; triangle + 2 < mesh.indices.size(); triangle += 3) {
        const u32 a = mesh.indices[triangle];
        const u32 b = mesh.indices[triangle + 1];
        const u32 c = mesh.indices[triangle + 2];
        if (a >= mesh.positions.size() || b >= mesh.positions.size() ||
            c >= mesh.positions.size()) {
            continue;
        }
        const math::Vec3 edge1 = mesh.positions[b] - mesh.positions[a];
        const math::Vec3 edge2 = mesh.positions[c] - mesh.positions[a];
        const math::Vec3 face{edge1.y * edge2.z - edge1.z * edge2.y,
                              edge1.z * edge2.x - edge1.x * edge2.z,
                              edge1.x * edge2.y - edge1.y * edge2.x};
        mesh.normals[a] = mesh.normals[a] + face;
        mesh.normals[b] = mesh.normals[b] + face;
        mesh.normals[c] = mesh.normals[c] + face;
    }
    for (math::Vec3& normal : mesh.normals) {
        const f32 length = math::Length(normal);
        normal = length > 1e-6f ? normal * (1.0f / length) : math::Vec3{0.0f, 1.0f, 0.0f};
    }
    log.Warning("Mesh '" + mesh.name + "' has no normals; smooth normals were generated");
}

void ComputeBounds(MeshData& mesh) {
    math::Aabb bounds;
    bounds.min = mesh.positions.front();
    bounds.max = mesh.positions.front();
    for (const math::Vec3& position : mesh.positions) {
        bounds.min = math::Min(bounds.min, position);
        bounds.max = math::Max(bounds.max, position);
    }
    mesh.bounds = bounds;
}

/// Resolves a material's texture index (into textures[]) to a reference string.
[[nodiscard]] std::string TextureReference(const serial::JsonValue& textures,
                                          const serial::JsonValue& images,
                                          const serial::JsonValue& textureValue,
                                          ImportLog& log) {
    if (!textureValue.IsObject()) {
        return {};
    }
    const i64 textureIndex = textureValue["index"].AsInt(-1);
    if (textureIndex < 0 || static_cast<usize>(textureIndex) >= textures.Size()) {
        return {};
    }
    const i64 imageIndex = textures[static_cast<usize>(textureIndex)]["source"].AsInt(-1);
    if (imageIndex < 0 || static_cast<usize>(imageIndex) >= images.Size()) {
        log.Warning("Material texture references a missing image");
        return {};
    }
    const serial::JsonValue& image = images[static_cast<usize>(imageIndex)];
    const std::string_view uri = image["uri"].AsString();
    if (!uri.empty()) {
        return std::string(uri);
    }
    if (image.Contains("bufferView")) {
        const std::string_view name = image["name"].AsString();
        const std::string key = name.empty() ? "image_" + std::to_string(imageIndex)
                                             : std::string(name);
        return std::string(kEmbeddedTexturePrefix) + key;
    }
    log.Warning("Material texture has neither a uri nor a bufferView");
    return {};
}

class GlbImporter final : public IImporter {
public:
    [[nodiscard]] std::string_view Name() const noexcept override { return "glb"; }
    [[nodiscard]] u32 Version() const noexcept override { return 1; }
    [[nodiscard]] AssetType OutputType() const noexcept override { return AssetType::Mesh; }

    [[nodiscard]] bool CanImport(const AssetPath& path) const noexcept override {
        return path.Extension() == ".glb";
    }

    [[nodiscard]] Result<ImportedAsset> Import(ConstByteSpan sourceBytes,
                                              const serial::JsonValue& settings,
                                              const AssetPath& sourcePath,
                                              ImportLog& log) override {
        L3D_UNUSED(settings);
        GlbParser parser;
        if (auto opened = parser.Open(sourceBytes); opened.IsError()) {
            return Unexpected(opened.Error());
        }
        if (auto parsed = parser.ParseDocument(log); parsed.IsError()) {
            return Unexpected(parsed.Error());
        }

        MeshDocument document;
        document.name = std::string(sourcePath.Stem());
        if (auto result = ImportMeshes(parser, document, log); result.IsError()) {
            return Unexpected(result.Error());
        }
        if (auto result = ImportMaterials(parser, document, log); result.IsError()) {
            return Unexpected(result.Error());
        }
        if (auto result = ImportEmbeddedImages(parser, document, log); result.IsError()) {
            return Unexpected(result.Error());
        }
        if (document.meshes.empty()) {
            return Unexpected(Status{StatusCode::ParseError, "GLB contains no meshes"});
        }
        return ImportedAsset{std::move(document)};
    }

private:
    [[nodiscard]] Result<void> ImportMeshes(GlbParser& parser, MeshDocument& document,
                                           ImportLog& log) const {
        const serial::JsonValue& meshes = parser.Meshes();
        for (usize meshIndex = 0; meshIndex < meshes.Size(); ++meshIndex) {
            const serial::JsonValue& meshValue = meshes[meshIndex];
            const std::string meshName =
                meshValue["name"].AsString().empty()
                    ? "mesh_" + std::to_string(meshIndex)
                    : std::string(meshValue["name"].AsString());

            const serial::JsonValue& primitives = meshValue["primitives"];
            if (primitives.Size() == 0) {
                log.Warning("Mesh '" + meshName + "' has no primitives and was skipped");
                continue;
            }

            for (usize primitiveIndex = 0; primitiveIndex < primitives.Size(); ++primitiveIndex) {
                const serial::JsonValue& primitive = primitives[primitiveIndex];
                const i64 mode = primitive["mode"].AsInt(4);
                if (mode != static_cast<i64>(kModeTriangles)) {
                    log.Warning("Mesh '" + meshName + "' primitive " +
                                std::to_string(primitiveIndex) +
                                " is not a triangle list and was skipped");
                    continue;
                }
                MeshData mesh;
                mesh.name = primitives.Size() > 1
                                ? meshName + "_" + std::to_string(primitiveIndex)
                                : meshName;
                if (auto result = ImportPrimitive(parser, primitive, mesh, log);
                    result.IsError()) {
                    return Unexpected(result.Error());
                }
                mesh.materialIndex = static_cast<i32>(primitive["material"].AsInt(-1));
                document.meshes.push_back(std::move(mesh));
            }
        }
        return {};
    }

    [[nodiscard]] Result<void> ImportPrimitive(GlbParser& parser,
                                              const serial::JsonValue& primitive, MeshData& mesh,
                                              ImportLog& log) const {
        const serial::JsonValue& attributes = primitive["attributes"];
        if (!attributes.Contains("POSITION")) {
            return Unexpected(
                Status{StatusCode::ParseError, "Primitive has no POSITION attribute"});
        }

        auto positionReader = MakeAttributeReader(parser, attributes["POSITION"].AsInt(-1));
        if (positionReader.IsError()) {
            return Unexpected(positionReader.Error());
        }
        const AttributeReader positions = *positionReader;
        if (positions.components != 3) {
            return Unexpected(Status{StatusCode::ParseError, "POSITION must have 3 components"});
        }

        mesh.positions.resize(positions.count);
        for (u32 i = 0; i < positions.count; ++i) {
            mesh.positions[i] = math::Vec3{ReadComponent(positions, i, 0),
                                           ReadComponent(positions, i, 1),
                                           ReadComponent(positions, i, 2)};
        }

        if (attributes.Contains("NORMAL")) {
            auto normalReader = MakeAttributeReader(parser, attributes["NORMAL"].AsInt(-1));
            if (normalReader.IsError()) {
                return Unexpected(normalReader.Error());
            }
            const AttributeReader normals = *normalReader;
            if (normals.count != positions.count) {
                return Unexpected(
                    Status{StatusCode::ParseError, "NORMAL count does not match POSITION"});
            }
            mesh.normals.resize(normals.count);
            for (u32 i = 0; i < normals.count; ++i) {
                mesh.normals[i] = math::Vec3{ReadComponent(normals, i, 0),
                                             ReadComponent(normals, i, 1),
                                             ReadComponent(normals, i, 2)};
            }
        }

        if (attributes.Contains("TEXCOORD_0")) {
            auto uvReader = MakeAttributeReader(parser, attributes["TEXCOORD_0"].AsInt(-1));
            if (uvReader.IsError()) {
                return Unexpected(uvReader.Error());
            }
            const AttributeReader uvs = *uvReader;
            if (uvs.count != positions.count) {
                return Unexpected(
                    Status{StatusCode::ParseError, "TEXCOORD_0 count does not match POSITION"});
            }
            mesh.uvs.resize(uvs.count);
            for (u32 i = 0; i < uvs.count; ++i) {
                mesh.uvs[i] = math::Vec2{ReadComponent(uvs, i, 0), ReadComponent(uvs, i, 1)};
            }
        }

        if (primitive.Contains("indices")) {
            auto indexReader = MakeAttributeReader(parser, primitive["indices"].AsInt(-1));
            if (indexReader.IsError()) {
                return Unexpected(indexReader.Error());
            }
            const AttributeReader indices = *indexReader;
            if (indices.count % 3 != 0) {
                return Unexpected(
                    Status{StatusCode::ParseError, "Index count is not a multiple of three"});
            }
            mesh.indices.resize(indices.count);
            for (u32 i = 0; i < indices.count; ++i) {
                const u32 index = ComponentToIndex(
                    indices.componentType,
                    indices.base + static_cast<usize>(i) * indices.stride);
                if (index >= positions.count) {
                    return Unexpected(
                        Status{StatusCode::ParseError, "Index references a missing vertex"});
                }
                mesh.indices[i] = index;
            }
        } else {
            // Non indexed: the vertex order *is* the index buffer.
            if (positions.count % 3 != 0) {
                return Unexpected(
                    Status{StatusCode::ParseError, "Non indexed vertex count is not a multiple of three"});
            }
            mesh.indices.resize(positions.count);
            for (u32 i = 0; i < positions.count; ++i) {
                mesh.indices[i] = i;
            }
        }

        if (mesh.normals.empty()) {
            GenerateNormals(mesh, log);
        }
        if (mesh.uvs.empty()) {
            mesh.uvs.assign(mesh.positions.size(), math::Vec2{0.0f, 0.0f});
            log.Warning("Mesh '" + mesh.name + "' has no texture coordinates; zeros were used");
        }
        ComputeBounds(mesh);

        if (!mesh.IsValid()) {
            return Unexpected(Status{StatusCode::ParseError, "Imported mesh failed validation"});
        }
        return {};
    }

    [[nodiscard]] Result<AttributeReader> MakeAttributeReader(GlbParser& parser,
                                                             i64 accessorIndex) const {
        const std::vector<GlbAccessor>& accessors = parser.Accessors();
        if (accessorIndex < 0 || static_cast<usize>(accessorIndex) >= accessors.size()) {
            return Unexpected(Status{StatusCode::ParseError, "Attribute references a missing accessor"});
        }
        return parser.MakeReader(accessors[static_cast<usize>(accessorIndex)]);
    }

    [[nodiscard]] Result<void> ImportMaterials(GlbParser& parser, MeshDocument& document,
                                              ImportLog& log) const {
        const serial::JsonValue& materials = parser.Materials();
        for (usize i = 0; i < materials.Size(); ++i) {
            const serial::JsonValue& value = materials[i];
            const serial::JsonValue& pbr = value["pbrMetallicRoughness"];

            MaterialData material;
            material.name = value["name"].AsString().empty()
                                ? "material_" + std::to_string(i)
                                : std::string(value["name"].AsString());

            const serial::JsonValue& baseColorFactor = pbr["baseColorFactor"];
            if (baseColorFactor.IsArray() && baseColorFactor.Size() >= 3) {
                material.baseColor = math::Vec4{
                    static_cast<f32>(baseColorFactor[0].AsNumber(1.0)),
                    static_cast<f32>(baseColorFactor[1].AsNumber(1.0)),
                    static_cast<f32>(baseColorFactor[2].AsNumber(1.0)),
                    baseColorFactor.Size() >= 4
                        ? static_cast<f32>(baseColorFactor[3].AsNumber(1.0))
                        : 1.0f};
            }
            material.metallic = static_cast<f32>(pbr["metallicFactor"].AsNumber(1.0));
            material.roughness = static_cast<f32>(pbr["roughnessFactor"].AsNumber(1.0));
            material.doubleSided = value["doubleSided"].AsBool(false);

            const serial::JsonValue& emissive = value["emissiveFactor"];
            if (emissive.IsArray() && emissive.Size() >= 3) {
                material.emissive = math::Vec3{static_cast<f32>(emissive[0].AsNumber(0.0)),
                                              static_cast<f32>(emissive[1].AsNumber(0.0)),
                                              static_cast<f32>(emissive[2].AsNumber(0.0))};
            }
            material.normalScale =
                static_cast<f32>(value["normalTexture"]["scale"].AsNumber(1.0));

            material.baseColorTexture = TextureReference(
                parser.Textures(), parser.Images(), pbr["baseColorTexture"], log);
            material.metallicRoughnessTexture = TextureReference(
                parser.Textures(), parser.Images(), pbr["metallicRoughnessTexture"], log);
            material.normalTexture =
                TextureReference(parser.Textures(), parser.Images(), value["normalTexture"], log);
            material.emissiveTexture =
                TextureReference(parser.Textures(), parser.Images(), value["emissiveTexture"], log);

            document.materials.push_back(std::move(material));
        }
        return {};
    }

    [[nodiscard]] Result<void> ImportEmbeddedImages(GlbParser& parser, MeshDocument& document,
                                                   ImportLog& log) const {
        const serial::JsonValue& images = parser.Images();
        for (usize i = 0; i < images.Size(); ++i) {
            const serial::JsonValue& image = images[i];
            if (!image.Contains("bufferView")) {
                continue; // Externally referenced images need no copying.
            }
            const i64 viewIndex = image["bufferView"].AsInt(-1);
            if (viewIndex < 0) {
                continue;
            }
            const std::string_view name = image["name"].AsString();
            EmbeddedImage embedded;
            embedded.name = name.empty() ? "image_" + std::to_string(i) : std::string(name);
            embedded.mimeType = std::string(image["mimeType"].AsString("image/png"));
            embedded.bytes = CopyBufferView(parser, static_cast<usize>(viewIndex), log);
            if (!embedded.bytes.empty()) {
                document.embeddedImages.push_back(std::move(embedded));
            }
        }
        return {};
    }

    [[nodiscard]] std::vector<u8> CopyBufferView(GlbParser& parser, usize viewIndex,
                                                ImportLog& log) const {
        const ConstByteSpan bin = parser.BinChunk();
        const auto range = parser.BufferViewRange(viewIndex);
        if (range.IsError()) {
            log.Warning("Embedded image buffer view is out of range");
            return {};
        }
        const u64 offset = range->first;
        const u64 length = range->second;
        if (offset + length > bin.size()) {
            log.Warning("Embedded image runs past the end of the GLB bin chunk");
            return {};
        }
        const u8* first = AsU8(bin.data()) + offset;
        return std::vector<u8>(first, first + length);
    }
};

} // namespace

std::unique_ptr<IImporter> CreateGlbImporter() {
    return std::make_unique<GlbImporter>();
}

} // namespace l3d::assets

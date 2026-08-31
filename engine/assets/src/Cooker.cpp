#include "local3d/assets/Cooker.hpp"

#include "local3d/core/Log.hpp"

#include <algorithm>
#include <unordered_set>

namespace l3d::assets {

namespace {

/// The first sub-asset of the document's primary type keeps the source id, so
/// "load the asset at this path" always resolves.  Later sub-assets get
/// deterministic derived ids, which is what makes cooked file names stable.
[[nodiscard]] AssetId SubAssetId(AssetId source, u32 index) {
    return index == 0 ? source : source.Sub(index);
}

[[nodiscard]] ConstByteSpan AsSpan(const std::vector<u8>& bytes) noexcept {
    return std::as_bytes(std::span(bytes.data(), bytes.size()));
}

/// Rejects counts that would make the reader allocate gigabytes from a
/// truncated or hostile file.  Generous, but bounded.
constexpr u64 kMaxReasonableCount = 64ULL * 1024ULL * 1024ULL;

[[nodiscard]] bool SaneCount(u64 count) noexcept { return count <= kMaxReasonableCount; }

/// Extension for an embedded image's MIME type, so it can go back through the
/// image importer exactly like a file on disk would.
[[nodiscard]] std::string ExtensionForMimeType(std::string_view mimeType) {
    if (mimeType == "image/jpeg" || mimeType == "image/jpg") {
        return "jpg";
    }
    if (mimeType == "image/bmp") {
        return "bmp";
    }
    if (mimeType == "image/tga" || mimeType == "image/x-tga") {
        return "tga";
    }
    return "png";
}

/// The settings recorded in the sidecar are the ones the import must use,
/// otherwise a user's edits in the editor would be cooked away.
[[nodiscard]] serial::JsonValue LoadSettings(IFileSystem& fs, const AssetPath& path) {
    if (auto meta = AssetMeta::Load(fs, path); meta.HasValue()) {
        return meta->settings;
    }
    return serial::JsonValue::MakeObject();
}

} // namespace

std::string_view CookedExtension(AssetType type) noexcept {
    switch (type) {
        case AssetType::Mesh: return ".l3dmesh";
        case AssetType::Texture: return ".l3dtex";
        case AssetType::AudioClip: return ".l3daudio";
        case AssetType::Material: return ".l3dmat";
        default: return ".l3dasset";
    }
}

std::string CookedFileName(AssetId id, AssetType type) {
    return id.ToString() + std::string(CookedExtension(type));
}

// --- Headers ---------------------------------------------------------------

void WriteCookedHeader(serial::BinaryWriter& writer, u32 tag, u64 sourceHash, u64 importHash) {
    writer.WriteU32(tag);
    writer.WriteU32(kCookedFormatVersion);
    writer.WriteU64(sourceHash);
    writer.WriteU64(importHash);
}

Result<void> ReadCookedHeader(serial::BinaryReader& reader, u32 expectedTag) {
    const u32 tag = reader.ReadU32();
    if (reader.HasError()) {
        return Unexpected(Status{StatusCode::ParseError, "Cooked file is too small for a header"});
    }
    if (tag != expectedTag) {
        return Unexpected(Status{StatusCode::ParseError, "Cooked file has the wrong type tag"});
    }
    const u32 version = reader.ReadU32();
    if (version != kCookedFormatVersion) {
        return Unexpected(Status{StatusCode::Unsupported, "Cooked format version is not supported"});
    }
    // Source hash and import fingerprint: recorded for diagnostics and checked
    // by the manifest, not by the reader.
    const u64 recordedSourceHash = reader.ReadU64();
    const u64 recordedImportHash = reader.ReadU64();
    L3D_UNUSED(recordedSourceHash);
    L3D_UNUSED(recordedImportHash);
    if (reader.HasError()) {
        return Unexpected(Status{StatusCode::ParseError, "Cooked header is truncated"});
    }
    return {};
}

// --- Meshes ----------------------------------------------------------------

Result<std::vector<u8>> CookMeshDocument(const MeshDocument& document, u64 sourceHash,
                                         u64 importHash) {
    serial::BinaryWriter writer;
    WriteCookedHeader(writer, kCookedMeshTag, sourceHash, importHash);
    writer.WriteString(document.name);
    writer.WriteVarUint(document.meshes.size());

    for (const MeshData& mesh : document.meshes) {
        if (!mesh.IsValid()) {
            return Unexpected(Status{StatusCode::InvalidArgument,
                                     "Mesh '" + mesh.name + "' is incomplete"});
        }
        writer.WriteString(mesh.name);
        writer.WriteF32(mesh.bounds.min.x);
        writer.WriteF32(mesh.bounds.min.y);
        writer.WriteF32(mesh.bounds.min.z);
        writer.WriteF32(mesh.bounds.max.x);
        writer.WriteF32(mesh.bounds.max.y);
        writer.WriteF32(mesh.bounds.max.z);
        writer.WriteI32(mesh.materialIndex);

        writer.WriteVarUint(mesh.positions.size());
        for (usize i = 0; i < mesh.positions.size(); ++i) {
            writer.WriteF32(mesh.positions[i].x);
            writer.WriteF32(mesh.positions[i].y);
            writer.WriteF32(mesh.positions[i].z);
            writer.WriteF32(mesh.normals[i].x);
            writer.WriteF32(mesh.normals[i].y);
            writer.WriteF32(mesh.normals[i].z);
            writer.WriteF32(mesh.uvs[i].x);
            writer.WriteF32(mesh.uvs[i].y);
        }

        writer.WriteVarUint(mesh.indices.size());
        for (const u32 index : mesh.indices) {
            writer.WriteU32(index);
        }
    }
    return writer.TakeBytes();
}

Result<MeshDocument> ReadMeshDocument(ConstByteSpan cooked) {
    serial::BinaryReader reader(cooked);
    if (auto header = ReadCookedHeader(reader, kCookedMeshTag); header.IsError()) {
        return Unexpected(header.Error());
    }

    MeshDocument document;
    document.name = reader.ReadString();
    const u64 meshCount = reader.ReadVarUint();
    if (reader.HasError() || !SaneCount(meshCount)) {
        return Unexpected(Status{StatusCode::ParseError, "Cooked mesh header is corrupt"});
    }

    document.meshes.reserve(static_cast<usize>(meshCount));
    for (u64 i = 0; i < meshCount; ++i) {
        MeshData mesh;
        mesh.name = reader.ReadString();
        mesh.bounds.min.x = reader.ReadF32();
        mesh.bounds.min.y = reader.ReadF32();
        mesh.bounds.min.z = reader.ReadF32();
        mesh.bounds.max.x = reader.ReadF32();
        mesh.bounds.max.y = reader.ReadF32();
        mesh.bounds.max.z = reader.ReadF32();
        mesh.materialIndex = reader.ReadI32();

        const u64 vertexCount = reader.ReadVarUint();
        if (reader.HasError() || !SaneCount(vertexCount)) {
            return Unexpected(Status{StatusCode::ParseError, "Cooked mesh vertex count is corrupt"});
        }
        mesh.positions.resize(static_cast<usize>(vertexCount));
        mesh.normals.resize(static_cast<usize>(vertexCount));
        mesh.uvs.resize(static_cast<usize>(vertexCount));
        for (u64 v = 0; v < vertexCount; ++v) {
            const usize at = static_cast<usize>(v);
            mesh.positions[at].x = reader.ReadF32();
            mesh.positions[at].y = reader.ReadF32();
            mesh.positions[at].z = reader.ReadF32();
            mesh.normals[at].x = reader.ReadF32();
            mesh.normals[at].y = reader.ReadF32();
            mesh.normals[at].z = reader.ReadF32();
            mesh.uvs[at].x = reader.ReadF32();
            mesh.uvs[at].y = reader.ReadF32();
        }

        const u64 indexCount = reader.ReadVarUint();
        if (reader.HasError() || !SaneCount(indexCount)) {
            return Unexpected(Status{StatusCode::ParseError, "Cooked mesh index count is corrupt"});
        }
        mesh.indices.resize(static_cast<usize>(indexCount));
        for (u64 n = 0; n < indexCount; ++n) {
            mesh.indices[static_cast<usize>(n)] = reader.ReadU32();
        }
        if (reader.HasError()) {
            return Unexpected(Status{StatusCode::ParseError, "Cooked mesh data is truncated"});
        }
        if (!mesh.IsValid()) {
            return Unexpected(Status{StatusCode::ParseError, "Cooked mesh failed validation"});
        }
        document.meshes.push_back(std::move(mesh));
    }
    return document;
}

// --- Textures --------------------------------------------------------------

Result<std::vector<u8>> CookTextureDocument(const TextureDocument& document, u64 sourceHash,
                                            u64 importHash) {
    if (!document.image.IsValid()) {
        return Unexpected(Status{StatusCode::InvalidArgument, "Image data is incomplete"});
    }
    serial::BinaryWriter writer;
    WriteCookedHeader(writer, kCookedTextureTag, sourceHash, importHash);
    writer.WriteU32(document.image.width);
    writer.WriteU32(document.image.height);
    writer.WriteU32(document.image.mipLevels);
    writer.WriteU32(static_cast<u32>(document.image.format));
    writer.WriteU8(document.settings.srgb ? 1 : 0);
    writer.WriteU8(document.settings.generateMips ? 1 : 0);
    writer.WriteU32(document.settings.maxSize);
    writer.WriteU8(document.settings.alphaIsCoverage ? 1 : 0);
    writer.WriteVarUint(document.image.pixels.size());
    writer.WriteBytes(AsSpan(document.image.pixels));
    return writer.TakeBytes();
}

Result<TextureDocument> ReadTextureDocument(ConstByteSpan cooked) {
    serial::BinaryReader reader(cooked);
    if (auto header = ReadCookedHeader(reader, kCookedTextureTag); header.IsError()) {
        return Unexpected(header.Error());
    }

    TextureDocument document;
    document.image.width = reader.ReadU32();
    document.image.height = reader.ReadU32();
    document.image.mipLevels = reader.ReadU32();
    const u32 formatValue = reader.ReadU32();
    if (formatValue >= static_cast<u32>(rhi::Format::Count)) {
        return Unexpected(Status{StatusCode::ParseError, "Cooked texture has an unknown format"});
    }
    document.image.format = static_cast<rhi::Format>(formatValue);
    document.settings.srgb = reader.ReadU8() != 0;
    document.settings.generateMips = reader.ReadU8() != 0;
    document.settings.maxSize = reader.ReadU32();
    document.settings.alphaIsCoverage = reader.ReadU8() != 0;

    const u64 byteCount = reader.ReadVarUint();
    if (reader.HasError() || !SaneCount(byteCount)) {
        return Unexpected(Status{StatusCode::ParseError, "Cooked texture size is corrupt"});
    }
    const ConstByteSpan pixels = reader.ReadBytes(static_cast<usize>(byteCount));
    if (reader.HasError()) {
        return Unexpected(Status{StatusCode::ParseError, "Cooked texture data is truncated"});
    }
    document.image.pixels.assign(reinterpret_cast<const u8*>(pixels.data()),
                                 reinterpret_cast<const u8*>(pixels.data()) + pixels.size());
    if (!document.image.IsValid()) {
        return Unexpected(Status{StatusCode::ParseError, "Cooked texture failed validation"});
    }
    return document;
}

// --- Audio -----------------------------------------------------------------

Result<std::vector<u8>> CookAudioDocument(const AudioDocument& document, u64 sourceHash,
                                          u64 importHash) {
    if (!document.audio.IsValid()) {
        return Unexpected(Status{StatusCode::InvalidArgument, "Audio data is incomplete"});
    }
    serial::BinaryWriter writer;
    WriteCookedHeader(writer, kCookedAudioTag, sourceHash, importHash);
    writer.WriteString(document.audio.name);
    writer.WriteU32(document.audio.sampleRate);
    writer.WriteU32(document.audio.channels);
    writer.WriteU32(document.audio.frameCount);
    writer.WriteVarUint(document.audio.samples.size());
    for (const f32 sample : document.audio.samples) {
        writer.WriteF32(sample);
    }
    return writer.TakeBytes();
}

Result<AudioDocument> ReadAudioDocument(ConstByteSpan cooked) {
    serial::BinaryReader reader(cooked);
    if (auto header = ReadCookedHeader(reader, kCookedAudioTag); header.IsError()) {
        return Unexpected(header.Error());
    }

    AudioDocument document;
    document.audio.name = reader.ReadString();
    document.audio.sampleRate = reader.ReadU32();
    document.audio.channels = reader.ReadU32();
    document.audio.frameCount = reader.ReadU32();
    const u64 sampleCount = reader.ReadVarUint();
    if (reader.HasError() || !SaneCount(sampleCount)) {
        return Unexpected(Status{StatusCode::ParseError, "Cooked audio sample count is corrupt"});
    }
    document.audio.samples.resize(static_cast<usize>(sampleCount));
    for (u64 i = 0; i < sampleCount; ++i) {
        document.audio.samples[static_cast<usize>(i)] = reader.ReadF32();
    }
    if (reader.HasError()) {
        return Unexpected(Status{StatusCode::ParseError, "Cooked audio data is truncated"});
    }
    if (!document.audio.IsValid()) {
        return Unexpected(Status{StatusCode::ParseError, "Cooked audio failed validation"});
    }
    return document;
}

// --- Materials -------------------------------------------------------------

AssetId CookedMaterial::TextureInSlot(u32 slot) const noexcept {
    switch (slot) {
        case 0: return baseColorTexture;
        case 1: return normalTexture;
        case 2: return metallicRoughnessTexture;
        case 3: return emissiveTexture;
        default: return kNullAssetId;
    }
}

Result<std::vector<u8>> CookMaterial(const CookedMaterial& material, u64 sourceHash,
                                     u64 importHash) {
    serial::BinaryWriter writer;
    WriteCookedHeader(writer, kCookedMaterialTag, sourceHash, importHash);
    writer.WriteString(material.data.name);
    writer.WriteF32(material.data.baseColor.x);
    writer.WriteF32(material.data.baseColor.y);
    writer.WriteF32(material.data.baseColor.z);
    writer.WriteF32(material.data.baseColor.w);
    writer.WriteF32(material.data.metallic);
    writer.WriteF32(material.data.roughness);
    writer.WriteF32(material.data.emissive.x);
    writer.WriteF32(material.data.emissive.y);
    writer.WriteF32(material.data.emissive.z);
    writer.WriteF32(material.data.normalScale);
    writer.WriteF32(material.data.alphaCutoff);
    writer.WriteU8(material.data.doubleSided ? 1 : 0);

    writer.WriteVarUint(CookedMaterial::kTextureSlots);
    for (u32 slot = 0; slot < CookedMaterial::kTextureSlots; ++slot) {
        const AssetId texture = material.TextureInSlot(slot);
        writer.WriteU8(texture.IsNull() ? 0 : 1);
        writer.WriteU64(texture.uuid.High());
        writer.WriteU64(texture.uuid.Low());
    }
    return writer.TakeBytes();
}

Result<CookedMaterial> ReadMaterial(ConstByteSpan cooked) {
    serial::BinaryReader reader(cooked);
    if (auto header = ReadCookedHeader(reader, kCookedMaterialTag); header.IsError()) {
        return Unexpected(header.Error());
    }

    CookedMaterial material;
    material.data.name = reader.ReadString();
    material.data.baseColor.x = reader.ReadF32();
    material.data.baseColor.y = reader.ReadF32();
    material.data.baseColor.z = reader.ReadF32();
    material.data.baseColor.w = reader.ReadF32();
    material.data.metallic = reader.ReadF32();
    material.data.roughness = reader.ReadF32();
    material.data.emissive.x = reader.ReadF32();
    material.data.emissive.y = reader.ReadF32();
    material.data.emissive.z = reader.ReadF32();
    material.data.normalScale = reader.ReadF32();
    material.data.alphaCutoff = reader.ReadF32();
    material.data.doubleSided = reader.ReadU8() != 0;

    const u64 slots = reader.ReadVarUint();
    if (reader.HasError() || slots > CookedMaterial::kTextureSlots) {
        return Unexpected(Status{StatusCode::ParseError, "Cooked material has too many texture slots"});
    }
    for (u64 slot = 0; slot < slots; ++slot) {
        const bool present = reader.ReadU8() != 0;
        const u64 high = reader.ReadU64();
        const u64 low = reader.ReadU64();
        if (present) {
            switch (slot) {
                case 0: material.baseColorTexture = AssetId{Uuid{high, low}}; break;
                case 1: material.normalTexture = AssetId{Uuid{high, low}}; break;
                case 2: material.metallicRoughnessTexture = AssetId{Uuid{high, low}}; break;
                default: material.emissiveTexture = AssetId{Uuid{high, low}}; break;
            }
        }
    }
    if (reader.HasError()) {
        return Unexpected(Status{StatusCode::ParseError, "Cooked material is truncated"});
    }
    return material;
}

// --- Manifest --------------------------------------------------------------

const CookedManifestEntry* CookedManifest::Find(AssetId id) const noexcept {
    for (const CookedManifestEntry& entry : entries) {
        if (entry.id == id) {
            return &entry;
        }
    }
    return nullptr;
}

void CookedManifest::Sort() {
    std::sort(entries.begin(), entries.end(),
              [](const CookedManifestEntry& a, const CookedManifestEntry& b) {
                  return a.id < b.id;
              });
}

std::string CookedManifest::Dump() const {
    serial::JsonValue root = serial::JsonValue::MakeObject();
    root.Set("formatVersion", formatVersion);
    serial::JsonValue list = serial::JsonValue::MakeArray();
    for (const CookedManifestEntry& entry : entries) {
        serial::JsonValue json = serial::JsonValue::MakeObject();
        json.Set("id", entry.id.ToString());
        json.Set("sourcePath", entry.sourcePath.ToString());
        json.Set("cookedPath", entry.cookedPath.ToString());
        json.Set("type", std::string(AssetTypeToString(entry.type)));
        json.Set("name", entry.name);
        serial::JsonValue deps = serial::JsonValue::MakeArray();
        for (const AssetId& dependency : entry.dependencies) {
            deps.Push(dependency.ToString());
        }
        json.Set("dependencies", std::move(deps));
        list.Push(std::move(json));
    }
    root.Set("entries", std::move(list));
    return root.Dump(2) + "\n";
}

Result<CookedManifest> CookedManifest::Parse(std::string_view jsonText) {
    auto parsed = serial::JsonValue::Parse(jsonText);
    if (parsed.IsError()) {
        return Unexpected(parsed.Error());
    }
    const serial::JsonValue& entries = (*parsed)["entries"];
    if (!entries.IsArray()) {
        return Unexpected(Status{StatusCode::ParseError, "Manifest has no entries array"});
    }

    CookedManifest manifest;
    manifest.formatVersion = static_cast<u32>((*parsed)["formatVersion"].AsInt(kCookedFormatVersion));
    if (manifest.formatVersion != kCookedFormatVersion) {
        return Unexpected(Status{StatusCode::Unsupported, "Manifest format version is not supported"});
    }
    manifest.entries.reserve(entries.Size());
    for (const serial::JsonValue& json : entries.AsArray()) {
        auto id = AssetId::Parse(json["id"].AsString());
        if (id.IsError()) {
            return Unexpected(id.Error());
        }
        CookedManifestEntry entry;
        entry.id = *id;
        entry.sourcePath = AssetPath{json["sourcePath"].AsString()};
        entry.cookedPath = AssetPath{json["cookedPath"].AsString()};
        entry.type = AssetTypeFromString(json["type"].AsString("Unknown"));
        entry.name = std::string(json["name"].AsString());
        for (const serial::JsonValue& dependency : json["dependencies"].AsArray()) {
            auto dependencyId = AssetId::Parse(dependency.AsString());
            if (dependencyId.IsError()) {
                return Unexpected(dependencyId.Error());
            }
            entry.dependencies.push_back(*dependencyId);
        }
        manifest.entries.push_back(std::move(entry));
    }
    return manifest;
}

// --- Cooker ----------------------------------------------------------------

Result<void> Cooker::Initialize(IFileSystem& sourceFs, IFileSystem& cookedFs,
                                AssetDatabase& database, const ImporterRegistry& importers) {
    sourceFs_ = &sourceFs;
    cookedFs_ = &cookedFs;
    database_ = &database;
    importers_ = &importers;
    manifest_ = CookedManifest{};
    initialized_ = true;
    return {};
}

Result<std::vector<CookedAsset>> Cooker::CookRecord(const AssetRecord& record,
                                                    std::vector<std::string>& warnings) {
    if (!initialized_) {
        return Unexpected(Status{StatusCode::NotInitialized, "Cooker is not initialized"});
    }
    auto bytes = sourceFs_->ReadFile(record.path.Text());
    if (bytes.IsError()) {
        return Unexpected(bytes.Error());
    }
    const ConstByteSpan source = AsSpan(*bytes);

    switch (record.type) {
        case AssetType::Mesh: return CookMeshSource(record, source, warnings);
        case AssetType::Texture: return CookTextureSource(record, source, warnings);
        case AssetType::AudioClip: return CookAudioSource(record, source, warnings);
        default:
            return Unexpected(Status{StatusCode::Unsupported,
                                     "No cooker for this asset type"});
    }
}

Result<std::vector<CookedAsset>> Cooker::CookMeshSource(const AssetRecord& record,
                                                        ConstByteSpan bytes,
                                                        std::vector<std::string>& warnings) {
    IImporter* importer = importers_->FindFor(record.path);
    if (importer == nullptr) {
        return Unexpected(Status{StatusCode::Unsupported, "No importer for this mesh source"});
    }
    ImportLog log;
    const serial::JsonValue settings = LoadSettings(*sourceFs_, record.path);
    auto imported = importer->Import(bytes, settings, record.path, log);
    for (const std::string& warning : log.Warnings()) {
        warnings.push_back(record.path.ToString() + ": " + warning);
    }
    if (imported.IsError()) {
        return Unexpected(imported.Error());
    }
    if (!std::holds_alternative<MeshDocument>(*imported)) {
        return Unexpected(Status{StatusCode::Internal, "Mesh importer returned the wrong document"});
    }
    const MeshDocument& document = std::get<MeshDocument>(*imported);

    const u32 meshCount = static_cast<u32>(document.meshes.size());
    const u32 materialCount = static_cast<u32>(document.materials.size());
    const u32 embeddedCount = static_cast<u32>(document.embeddedImages.size());

    std::vector<CookedAsset> output;
    const u64 sourceHash = record.sourceHash;
    const u64 importHash = record.ImportFingerprint();

    // Embedded images first: materials need their ids, and the sub id layout
    // (meshes, then materials, then embedded images) is fixed by this order.
    std::vector<AssetId> embeddedIds;
    embeddedIds.reserve(embeddedCount);
    for (u32 i = 0; i < embeddedCount; ++i) {
        embeddedIds.push_back(SubAssetId(record.id, meshCount + materialCount + i));
    }

    for (u32 i = 0; i < meshCount; ++i) {
        MeshDocument single;
        single.name = document.name;
        single.meshes.push_back(document.meshes[i]);

        auto cooked = CookMeshDocument(single, sourceHash, importHash);
        if (cooked.IsError()) {
            return Unexpected(cooked.Error());
        }
        CookedAsset asset;
        asset.id = SubAssetId(record.id, i);
        asset.type = AssetType::Mesh;
        asset.name = document.meshes[i].name;
        asset.path = AssetPath{CookedFileName(asset.id, asset.type)};
        asset.bytes = std::move(*cooked);
        output.push_back(std::move(asset));
    }

    for (u32 i = 0; i < materialCount; ++i) {
        const MaterialData& material = document.materials[i];
        CookedMaterial cookedMaterial;
        cookedMaterial.data = material;

        auto resolve = [&](const std::string& reference) {
            if (reference.empty()) {
                return kNullAssetId;
            }
            const u32 base = meshCount + materialCount;
            return ResolveTextureReference(record, document, reference, base, warnings);
        };
        cookedMaterial.baseColorTexture = resolve(material.baseColorTexture);
        cookedMaterial.normalTexture = resolve(material.normalTexture);
        cookedMaterial.metallicRoughnessTexture = resolve(material.metallicRoughnessTexture);
        cookedMaterial.emissiveTexture = resolve(material.emissiveTexture);

        auto cooked = CookMaterial(cookedMaterial, sourceHash, importHash);
        if (cooked.IsError()) {
            return Unexpected(cooked.Error());
        }
        CookedAsset asset;
        asset.id = SubAssetId(record.id, meshCount + i);
        asset.type = AssetType::Material;
        asset.name = material.name;
        asset.path = AssetPath{CookedFileName(asset.id, asset.type)};
        asset.bytes = std::move(*cooked);
        for (u32 slot = 0; slot < CookedMaterial::kTextureSlots; ++slot) {
            const AssetId texture = cookedMaterial.TextureInSlot(slot);
            if (!texture.IsNull()) {
                asset.dependencies.push_back(texture);
            }
        }
        output.push_back(std::move(asset));
    }

    // Embedded images become real texture assets with derived ids.
    for (u32 i = 0; i < embeddedCount; ++i) {
        const EmbeddedImage& embedded = document.embeddedImages[i];
        TextureDocument textureDocument;
        textureDocument.settings.srgb = true;
        textureDocument.settings.generateMips = true;
        ImportLog imageLog;
        IImporter* imageImporter = importers_->FindFor(
            AssetPath{"embedded." + ExtensionForMimeType(embedded.mimeType)});
        if (imageImporter == nullptr) {
            warnings.push_back(record.path.ToString() + ": no importer for embedded image '" +
                               embedded.name + "'");
            continue;
        }
        auto importedImage = imageImporter->Import(
            AsSpan(embedded.bytes), TextureSettingsToJson(textureDocument.settings),
            AssetPath{embedded.name + "." + ExtensionForMimeType(embedded.mimeType)}, imageLog);
        for (const std::string& warning : imageLog.Warnings()) {
            warnings.push_back(record.path.ToString() + "/" + embedded.name + ": " + warning);
        }
        if (importedImage.IsError() || !std::holds_alternative<TextureDocument>(*importedImage)) {
            warnings.push_back(record.path.ToString() + ": embedded image '" + embedded.name +
                               "' could not be decoded");
            continue;
        }
        auto cooked = CookTextureDocument(std::get<TextureDocument>(*importedImage), sourceHash,
                                          importHash);
        if (cooked.IsError()) {
            return Unexpected(cooked.Error());
        }
        CookedAsset asset;
        asset.id = embeddedIds[i];
        asset.type = AssetType::Texture;
        asset.name = embedded.name;
        asset.path = AssetPath{CookedFileName(asset.id, asset.type)};
        asset.bytes = std::move(*cooked);
        output.push_back(std::move(asset));
    }

    if (output.empty()) {
        return Unexpected(Status{StatusCode::ParseError, "Mesh source produced no assets"});
    }
    return output;
}

Result<std::vector<CookedAsset>> Cooker::CookTextureSource(const AssetRecord& record,
                                                           ConstByteSpan bytes,
                                                           std::vector<std::string>& warnings) {
    IImporter* importer = importers_->FindFor(record.path);
    if (importer == nullptr) {
        return Unexpected(Status{StatusCode::Unsupported, "No importer for this image source"});
    }
    ImportLog log;
    const serial::JsonValue settings = LoadSettings(*sourceFs_, record.path);
    auto imported = importer->Import(bytes, settings, record.path, log);
    for (const std::string& warning : log.Warnings()) {
        warnings.push_back(record.path.ToString() + ": " + warning);
    }
    if (imported.IsError() || !std::holds_alternative<TextureDocument>(*imported)) {
        return imported.IsError()
                   ? Unexpected(imported.Error())
                   : Unexpected(Status{StatusCode::Internal,
                                       "Image importer returned the wrong document"});
    }

    auto cooked = CookTextureDocument(std::get<TextureDocument>(*imported), record.sourceHash,
                                      record.ImportFingerprint());
    if (cooked.IsError()) {
        return Unexpected(cooked.Error());
    }
    CookedAsset asset;
    asset.id = record.id;
    asset.type = AssetType::Texture;
    asset.name = std::string(record.path.Stem());
    asset.path = AssetPath{CookedFileName(asset.id, asset.type)};
    asset.bytes = std::move(*cooked);
    return std::vector<CookedAsset>{std::move(asset)};
}

Result<std::vector<CookedAsset>> Cooker::CookAudioSource(const AssetRecord& record,
                                                         ConstByteSpan bytes,
                                                         std::vector<std::string>& warnings) {
    IImporter* importer = importers_->FindFor(record.path);
    if (importer == nullptr) {
        return Unexpected(Status{StatusCode::Unsupported, "No importer for this audio source"});
    }
    ImportLog log;
    const serial::JsonValue settings = LoadSettings(*sourceFs_, record.path);
    auto imported = importer->Import(bytes, settings, record.path, log);
    for (const std::string& warning : log.Warnings()) {
        warnings.push_back(record.path.ToString() + ": " + warning);
    }
    if (imported.IsError() || !std::holds_alternative<AudioDocument>(*imported)) {
        return imported.IsError()
                   ? Unexpected(imported.Error())
                   : Unexpected(Status{StatusCode::Internal,
                                       "Audio importer returned the wrong document"});
    }

    auto cooked = CookAudioDocument(std::get<AudioDocument>(*imported), record.sourceHash,
                                    record.ImportFingerprint());
    if (cooked.IsError()) {
        return Unexpected(cooked.Error());
    }
    CookedAsset asset;
    asset.id = record.id;
    asset.type = AssetType::AudioClip;
    asset.name = std::get<AudioDocument>(*imported).audio.name;
    asset.path = AssetPath{CookedFileName(asset.id, asset.type)};
    asset.bytes = std::move(*cooked);
    return std::vector<CookedAsset>{std::move(asset)};
}

AssetId Cooker::ResolveTextureReference(const AssetRecord& record, const MeshDocument& document,
                                        std::string_view reference, u32 subIndexBase,
                                        std::vector<std::string>& warnings) const {
    if (reference.starts_with(kEmbeddedTexturePrefix)) {
        const std::string name(reference.substr(kEmbeddedTexturePrefix.size()));
        const i32 index = document.FindEmbeddedImage(name);
        if (index < 0) {
            warnings.push_back(record.path.ToString() + ": material references embedded image '" +
                               name + "' which is not in the file");
            return kNullAssetId;
        }
        return SubAssetId(record.id, subIndexBase + static_cast<u32>(index));
    }

    // An external reference is relative to the source file's directory.
    const AssetPath resolved = record.path.Parent().Child(std::string(reference));
    const AssetRecord* texture = database_->FindByPath(resolved);
    if (texture == nullptr) {
        warnings.push_back(record.path.ToString() + ": material references '" +
                           std::string(reference) + "' which is not in the asset database");
        return kNullAssetId;
    }
    return texture->id;
}

Result<CookReport> Cooker::CookAll() {
    if (!initialized_) {
        return Unexpected(Status{StatusCode::NotInitialized, "Cooker is not initialized"});
    }
    CookReport report;

    // Start from the manifest that is already there so assets we skip keep
    // their entries; a broken manifest just means we recook everything.
    if (auto text = ReadFileAsText(*cookedFs_, kManifestFileName); text.HasValue()) {
        auto parsed = CookedManifest::Parse(*text);
        if (parsed.HasValue()) {
            manifest_ = std::move(*parsed);
        } else {
            report.warnings.push_back("Existing manifest could not be parsed; recooking everything");
            manifest_ = CookedManifest{};
        }
    }

    std::unordered_set<AssetPath> liveSources;
    liveSources.reserve(database_->Count());
    for (const AssetRecord& record : database_->Records()) {
        liveSources.insert(record.path);
    }

    for (const AssetRecord& record : database_->Records()) {
        if (!record.NeedsImport()) {
            ++report.skipped;
            report.skippedPaths.push_back(record.path.ToString());
            continue;
        }
        auto cooked = CookRecord(record, report.warnings);
        if (cooked.IsError()) {
            ++report.failed;
            report.errors.push_back(record.path.ToString() + ": " +
                                    std::string(cooked.Error().Message()));
            continue;
        }
        ++report.sourceFilesCooked;
        report.cookedPaths.push_back(record.path.ToString());

        for (const CookedAsset& asset : *cooked) {
            if (auto written = cookedFs_->WriteFile(asset.path.Text(), AsSpan(asset.bytes));
                written.IsError()) {
                ++report.failed;
                report.errors.push_back("Could not write " + asset.path.ToString());
                continue;
            }
            ++report.filesWritten;

            CookedManifestEntry entry;
            entry.id = asset.id;
            entry.sourcePath = record.path;
            entry.cookedPath = asset.path;
            entry.type = asset.type;
            entry.name = asset.name;
            entry.dependencies = asset.dependencies;
            bool replaced = false;
            for (CookedManifestEntry& existing : manifest_.entries) {
                if (existing.id == entry.id) {
                    existing = entry;
                    replaced = true;
                    break;
                }
            }
            if (!replaced) {
                manifest_.entries.push_back(std::move(entry));
            }
        }

        if (auto marked = database_->MarkCooked(record.id, record.sourceHash,
                                                record.ImportFingerprint());
            marked.IsError()) {
            report.warnings.push_back("Could not record the cook of " + record.path.ToString());
        }
    }

    // Drop entries whose source file is gone, and remove their cooked files.
    std::vector<CookedManifestEntry> kept;
    kept.reserve(manifest_.entries.size());
    for (CookedManifestEntry& entry : manifest_.entries) {
        if (liveSources.contains(entry.sourcePath)) {
            kept.push_back(std::move(entry));
            continue;
        }
        if (auto removed = cookedFs_->Remove(entry.cookedPath.Text()); removed.IsError()) {
            report.warnings.push_back("Could not delete stale " + entry.cookedPath.ToString());
        }
    }
    manifest_.entries = std::move(kept);
    manifest_.Sort();

    const std::string text = manifest_.Dump();
    if (auto saved = cookedFs_->WriteFile(kManifestFileName,
                                          std::as_bytes(std::span(text.data(), text.size())));
        saved.IsError()) {
        return Unexpected(saved.Error());
    }
    return report;
}

} // namespace l3d::assets

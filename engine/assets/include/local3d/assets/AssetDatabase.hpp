#pragma once
/// @file AssetDatabase.hpp
/// @brief The editor side view of the asset folder: what exists, what it is,
///        what id it has and whether it needs reimporting.
///
/// The database owns identity assignment.  On a scan it walks the root, reads
/// every `.l3dmeta` sidecar and reports what was added, removed, modified or
/// moved.  Ids come from the sidecar when there is one, so a rename keeps its
/// id; when there is not, the content hash is compared against the files that
/// just disappeared, which recovers the id of a file that was moved without its
/// sidecar.  Only a genuinely new file gets a fresh id.
///
/// The database never imports and never touches the GPU.  It answers "what
/// should the cooker look at", and the cooker answers the rest.

#include "local3d/assets/AssetId.hpp"
#include "local3d/assets/AssetMeta.hpp"
#include "local3d/assets/FileSystem.hpp"
#include "local3d/assets/Importer.hpp"
#include "local3d/core/Common.hpp"
#include "local3d/core/Result.hpp"

#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace l3d::assets {

/// One imported source file, as the database sees it.
struct AssetRecord {
    AssetId id;
    AssetPath path;
    AssetType type = AssetType::Unknown;
    std::string importer;
    u32 importerVersion = 0;
    /// Hash of the importer settings block.
    u64 settingsHash = 0;
    /// Hash of the bytes currently on disk.
    u64 sourceHash = 0;
    /// Source hash and import fingerprint at the last successful cook.
    u64 cookedSourceHash = 0;
    u64 cookedImportHash = 0;
    /// True once a sidecar exists on disk for this record.
    bool hasSidecar = false;

    [[nodiscard]] u64 ImportFingerprint() const noexcept {
        return ImportFingerprintOf(importer, importerVersion, settingsHash);
    }

    /// True when the source bytes or the import inputs changed since the cook.
    [[nodiscard]] bool NeedsImport() const noexcept {
        return cookedSourceHash != sourceHash || cookedImportHash != ImportFingerprint();
    }
};

/// What a scan changed.  The path lists are sorted, so the editor can show them
/// in a stable order.
struct ScanReport {
    u32 added = 0;
    u32 removed = 0;
    u32 modified = 0;
    u32 unchanged = 0;
    /// Files whose id was recovered from a file that disappeared in the same
    /// scan (a move or a rename without its sidecar).
    u32 moved = 0;
    /// Files with no known extension or no importer.
    u32 skipped = 0;

    std::vector<std::string> addedPaths;
    std::vector<std::string> removedPaths;
    std::vector<std::string> modifiedPaths;
    std::vector<std::string> movedPaths;
    std::vector<std::string> warnings;

    [[nodiscard]] bool Empty() const noexcept {
        return added == 0 && removed == 0 && modified == 0 && moved == 0;
    }
};

struct AssetDatabaseDesc {
    /// Only used for logging and ToNativePath; the file system already knows
    /// where its root is.
    std::string rootName = "assets";
    /// When false the database is read only: useful for a runtime that ships
    /// cooked data and must never write into the project.
    bool writeSidecars = true;
};

class AssetDatabase {
public:
    [[nodiscard]] Result<void> Initialize(IFileSystem& fs, const AssetDatabaseDesc& desc);

    /// Walks the root and rebuilds the record table.  Idempotent: scanning an
    /// unchanged tree reports everything as unchanged.
    [[nodiscard]] Result<ScanReport> Scan(const ImporterRegistry& importers);

    [[nodiscard]] bool IsInitialized() const noexcept { return initialized_; }
    [[nodiscard]] usize Count() const noexcept { return records_.size(); }
    /// All records, sorted by path.
    [[nodiscard]] std::span<const AssetRecord> Records() const noexcept { return records_; }
    [[nodiscard]] const AssetRecord* FindById(AssetId id) const noexcept;
    [[nodiscard]] const AssetRecord* FindByPath(const AssetPath& path) const noexcept;
    [[nodiscard]] usize NeedsImportCount() const noexcept;

    /// Records a successful cook so the next scan sees the asset as up to date.
    /// Also rewrites the sidecar, which is where the cook state is persisted.
    [[nodiscard]] OperationResult MarkCooked(AssetId id, u64 sourceHash, u64 importFingerprint);

    /// Persists the table as `.l3dindex.json` so an editor can list assets
    /// without walking the tree.  The sidecars remain the source of truth; the
    /// index is only a cache.
    [[nodiscard]] OperationResult SaveIndex() const;
    [[nodiscard]] Result<usize> LoadIndex();

private:
    [[nodiscard]] OperationResult CollectFiles(std::vector<AssetPath>& out, const AssetPath& dir,
                                              std::vector<std::string>& warnings) const;

    IFileSystem* fs_ = nullptr;
    AssetDatabaseDesc desc_;
    bool initialized_ = false;
    std::vector<AssetRecord> records_;
    std::unordered_map<AssetId, usize> byId_;
    std::unordered_map<AssetPath, usize> byPath_;

    void RebuildIndices();
};

/// Name of the index cache inside the asset root.
inline constexpr std::string_view kIndexFileName = ".l3dindex.json";

} // namespace l3d::assets

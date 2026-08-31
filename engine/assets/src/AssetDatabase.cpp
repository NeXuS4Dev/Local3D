#include "local3d/assets/AssetDatabase.hpp"

#include "local3d/core/Hash.hpp"
#include "local3d/core/Log.hpp"

#include <algorithm>
#include <unordered_set>

namespace l3d::assets {

namespace {

/// Hashes are stored as hex strings in JSON: a JSON number is a double and
/// would silently lose the top bits of a 64 bit hash.
[[nodiscard]] std::string ToHex(u64 value) {
    std::string text;
    text.resize(16, '0');
    for (usize i = 0; i < 16; ++i) {
        const u32 nibble = static_cast<u32>((value >> ((15 - i) * 4)) & 0xF);
        text[i] = static_cast<char>(nibble < 10 ? '0' + nibble : 'a' + (nibble - 10));
    }
    return text;
}

[[nodiscard]] u64 FromHex(std::string_view text) {
    u64 value = 0;
    for (const char c : text) {
        value <<= 4;
        if (c >= '0' && c <= '9') {
            value |= static_cast<u64>(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            value |= static_cast<u64>(c - 'a' + 10);
        } else if (c >= 'A' && c <= 'F') {
            value |= static_cast<u64>(c - 'A' + 10);
        }
    }
    return value;
}

} // namespace

Result<void> AssetDatabase::Initialize(IFileSystem& fs, const AssetDatabaseDesc& desc) {
    fs_ = &fs;
    desc_ = desc;
    records_.clear();
    RebuildIndices();
    initialized_ = true;
    return {};
}

OperationResult AssetDatabase::CollectFiles(std::vector<AssetPath>& out, const AssetPath& dir,
                                            std::vector<std::string>& warnings) const {
    auto entries = fs_->ListDirectory(dir.Text());
    if (entries.IsError()) {
        return Unexpected(entries.Error());
    }
    std::vector<std::string> names = std::move(*entries);
    // Sorted so a scan visits files in the same order every time, which keeps
    // id assignment and cook output deterministic.
    std::sort(names.begin(), names.end());

    for (const std::string& name : names) {
        // Dot files are ours (.l3dindex.json) or the user's editor state.
        if (name.empty() || name.front() == '.') {
            continue;
        }
        const AssetPath child = dir.IsValid() ? dir.Child(name) : AssetPath{name};
        if (fs_->IsDirectory(child.Text())) {
            if (auto recursion = CollectFiles(out, child, warnings); recursion.IsError()) {
                return Unexpected(recursion.Error());
            }
            continue;
        }
        if (child.Extension() == kMetaExtension) {
            continue; // Sidecars describe their source file; they are not assets.
        }
        out.push_back(child);
    }
    return {};
}

Result<ScanReport> AssetDatabase::Scan(const ImporterRegistry& importers) {
    if (!initialized_) {
        return Unexpected(Status{StatusCode::NotInitialized, "Asset database is not initialized"});
    }

    ScanReport report;
    std::vector<AssetPath> files;
    if (auto collected = CollectFiles(files, AssetPath{}, report.warnings); collected.IsError()) {
        return Unexpected(collected.Error());
    }

    // Keep the previous table: removal and move detection both need it, and it
    // is what makes a second scan of an unchanged tree report no changes.
    std::vector<AssetRecord> previous = records_;
    std::unordered_map<AssetId, const AssetRecord*> previousById;
    for (const AssetRecord& record : previous) {
        previousById.emplace(record.id, &record);
    }

    // Which paths still exist?  AssetPath hashing and equality are case
    // insensitive, which is what we want for both lookups and conflict checks.
    std::unordered_map<AssetPath, std::string> seenPaths;
    for (const AssetPath& file : files) {
        const auto existing = seenPaths.find(file);
        if (existing != seenPaths.end() && existing->second != file.ToString()) {
            report.warnings.push_back("Paths '" + existing->second + "' and '" + file.ToString() +
                                      "' differ only in case; the first one wins");
            continue;
        }
        seenPaths.emplace(file, file.ToString());
    }

    // Files that disappeared this scan, indexed by the content hash they had.
    // A new file with a matching hash is a move, not an addition.
    std::unordered_map<u64, const AssetRecord*> vanishedByHash;
    for (const AssetRecord& record : previous) {
        if (seenPaths.contains(record.path)) {
            continue;
        }
        ++report.removed;
        report.removedPaths.push_back(record.path.ToString());
        vanishedByHash.emplace(record.sourceHash, &record);
    }
    std::sort(report.removedPaths.begin(), report.removedPaths.end());

    std::vector<AssetRecord> next;
    next.reserve(files.size());
    std::unordered_set<AssetId> seenIds;

    // A move is not a deletion: undo the removal the old path was counted as so
    // the editor does not throw away state that is still valid.
    auto undoRemoval = [&report](const AssetPath& oldPath) {
        if (report.removed > 0) {
            --report.removed;
        }
        const std::string text = oldPath.ToString();
        const auto stale =
            std::find(report.removedPaths.begin(), report.removedPaths.end(), text);
        if (stale != report.removedPaths.end()) {
            report.removedPaths.erase(stale);
        }
    };

    for (const AssetPath& file : files) {
        if (!seenPaths.contains(file) || seenPaths[file] != file.ToString()) {
            continue; // Lost a case conflict above.
        }

        const AssetType type = file.Type();
        IImporter* importer = importers.FindFor(file);
        if (type == AssetType::Unknown || importer == nullptr) {
            ++report.skipped;
            continue;
        }

        auto bytes = fs_->ReadFile(file.ToString());
        if (bytes.IsError()) {
            report.warnings.push_back("Could not read '" + file.ToString() + "': " +
                                      std::string(bytes.Error().Message()));
            ++report.skipped;
            continue;
        }

        AssetRecord record;
        record.path = file;
        record.type = type;
        record.sourceHash = HashBytes(std::as_bytes(std::span(*bytes)));
        record.importer = std::string(importer->Name());
        record.importerVersion = importer->Version();

        bool writeSidecar = false;
        bool justAdded = false;
        bool justMoved = false;
        auto meta = AssetMeta::Load(*fs_, file);
        if (meta.HasValue()) {
            record.id = meta->id;
            record.settingsHash = HashSettings(meta->settings);
            record.cookedSourceHash = meta->cookedSourceHash;
            record.cookedImportHash = meta->cookedImportHash;
            record.hasSidecar = true;
            if (meta->type != AssetType::Unknown) {
                record.type = meta->type;
            }
            if (record.id.IsNull()) {
                record.id = AssetId::Generate();
                writeSidecar = true;
                report.warnings.push_back("Sidecar for '" + file.ToString() + "' had no id; assigned one");
            }
            if (!meta->importer.empty() && meta->importer != record.importer) {
                report.warnings.push_back("Importer for '" + file.ToString() + "' changed from '" +
                                          meta->importer + "' to '" + record.importer + "'");
                writeSidecar = true;
            }
            if (meta->importerVersion != record.importerVersion) {
                writeSidecar = true;
            }
        } else {
            // No usable sidecar: a new file, a move, or a corrupt sidecar.
            writeSidecar = true;
            const auto moved = vanishedByHash.find(record.sourceHash);
            if (moved != vanishedByHash.end()) {
                record.id = moved->second->id;
                // Carry the cook state over: the bytes are identical, so the
                // cooked output is still valid and must not be rebuilt.
                record.cookedSourceHash = moved->second->cookedSourceHash;
                record.cookedImportHash = moved->second->cookedImportHash;
                record.settingsHash = moved->second->settingsHash;
                justMoved = true;
                undoRemoval(moved->second->path);
                vanishedByHash.erase(moved);
            } else {
                record.id = AssetId::Generate();
                record.settingsHash = HashSettings(serial::JsonValue::MakeObject());
                justAdded = true;
                if (meta.IsError() && meta.Error().Code() != StatusCode::NotFound) {
                    report.warnings.push_back("Sidecar for '" + file.ToString() +
                                              "' could not be read; a new one was written");
                }
            }
        }

        if (seenIds.contains(record.id)) {
            // Two files claiming one id - almost always a file copied together
            // with its sidecar.  This one gets a new id rather than both
            // silently aliasing, and it counts as the addition it is.
            report.warnings.push_back("'" + file.ToString() +
                                      "' shares its id with another asset; assigning a new one");
            record.id = AssetId::Generate();
            writeSidecar = true;
            justAdded = true;
        }
        seenIds.insert(record.id);

        if (!justAdded && !justMoved) {
            const auto previousRecord = previousById.find(record.id);
            if (previousRecord != previousById.end()) {
                const AssetPath& oldPath = previousRecord->second->path;
                if (oldPath != record.path && !seenPaths.contains(oldPath)) {
                    // The sidecar travelled with the file: same id, new path and
                    // the old path is gone, so this is a move and not a copy.
                    justMoved = true;
                    undoRemoval(oldPath);
                } else if (previousRecord->second->sourceHash != record.sourceHash) {
                    // Only the bytes changing counts as a modification, so an
                    // untouched tree reports nothing at all.
                    ++report.modified;
                    report.modifiedPaths.push_back(file.ToString());
                }
            }
        }

        if (justAdded) {
            ++report.added;
            report.addedPaths.push_back(file.ToString());
        } else if (justMoved) {
            ++report.moved;
            report.movedPaths.push_back(file.ToString());
        }

        if (writeSidecar && desc_.writeSidecars) {
            AssetMeta sidecar;
            sidecar.id = record.id;
            sidecar.type = record.type;
            sidecar.importer = record.importer;
            sidecar.importerVersion = record.importerVersion;
            sidecar.cookedSourceHash = record.cookedSourceHash;
            sidecar.cookedImportHash = record.cookedImportHash;
            if (auto saved = sidecar.Save(*fs_, file); saved.IsError()) {
                report.warnings.push_back("Could not write sidecar for '" + file.ToString() + "'");
            }
        }

        next.push_back(std::move(record));
    }

    std::sort(next.begin(), next.end(),
              [](const AssetRecord& a, const AssetRecord& b) { return a.path.Text() < b.path.Text(); });
    std::sort(report.addedPaths.begin(), report.addedPaths.end());
    std::sort(report.modifiedPaths.begin(), report.modifiedPaths.end());
    std::sort(report.movedPaths.begin(), report.movedPaths.end());

    for (const AssetRecord& record : next) {
        if (!record.NeedsImport()) {
            ++report.unchanged;
        }
    }

    records_ = std::move(next);
    RebuildIndices();
    return report;
}

const AssetRecord* AssetDatabase::FindById(AssetId id) const noexcept {
    const auto found = byId_.find(id);
    return found == byId_.end() ? nullptr : &records_[found->second];
}

const AssetRecord* AssetDatabase::FindByPath(const AssetPath& path) const noexcept {
    const auto found = byPath_.find(path);
    return found == byPath_.end() ? nullptr : &records_[found->second];
}

usize AssetDatabase::NeedsImportCount() const noexcept {
    usize count = 0;
    for (const AssetRecord& record : records_) {
        if (record.NeedsImport()) {
            ++count;
        }
    }
    return count;
}

OperationResult AssetDatabase::MarkCooked(AssetId id, u64 sourceHash, u64 importFingerprint) {
    AssetRecord* record = nullptr;
    const auto found = byId_.find(id);
    if (found != byId_.end()) {
        record = &records_[found->second];
    }
    if (record == nullptr) {
        return Unexpected(Status{StatusCode::NotFound, "Asset is not in the database"});
    }
    record->sourceHash = sourceHash;
    record->cookedSourceHash = sourceHash;
    record->cookedImportHash = importFingerprint;

    if (!desc_.writeSidecars || !fs_) {
        return {};
    }
    AssetMeta sidecar;
    sidecar.id = record->id;
    sidecar.type = record->type;
    sidecar.importer = record->importer;
    sidecar.importerVersion = record->importerVersion;
    sidecar.cookedSourceHash = record->cookedSourceHash;
    sidecar.cookedImportHash = record->cookedImportHash;
    return sidecar.Save(*fs_, record->path);
}

OperationResult AssetDatabase::SaveIndex() const {
    if (!initialized_) {
        return Unexpected(Status{StatusCode::NotInitialized, "Asset database is not initialized"});
    }
    serial::JsonValue root = serial::JsonValue::MakeObject();
    root.Set("formatVersion", 1);
    root.Set("root", desc_.rootName);
    serial::JsonValue list = serial::JsonValue::MakeArray();
    for (const AssetRecord& record : records_) {
        serial::JsonValue entry = serial::JsonValue::MakeObject();
        entry.Set("id", record.id.ToString());
        entry.Set("path", record.path.ToString());
        entry.Set("type", std::string(AssetTypeToString(record.type)));
        entry.Set("importer", record.importer);
        entry.Set("importerVersion", record.importerVersion);
        entry.Set("settingsHash", ToHex(record.settingsHash));
        entry.Set("sourceHash", ToHex(record.sourceHash));
        entry.Set("cookedSourceHash", ToHex(record.cookedSourceHash));
        entry.Set("cookedImportHash", ToHex(record.cookedImportHash));
        list.Push(std::move(entry));
    }
    root.Set("records", std::move(list));

    const std::string text = root.Dump(2) + "\n";
    return fs_->WriteFile(kIndexFileName, std::as_bytes(std::span(text.data(), text.size())));
}

Result<usize> AssetDatabase::LoadIndex() {
    if (!initialized_) {
        return Unexpected(Status{StatusCode::NotInitialized, "Asset database is not initialized"});
    }
    auto text = ReadFileAsText(*fs_, kIndexFileName);
    if (text.IsError()) {
        return Unexpected(text.Error());
    }
    auto parsed = serial::JsonValue::Parse(*text);
    if (parsed.IsError()) {
        return Unexpected(parsed.Error());
    }
    const serial::JsonValue& records = (*parsed)["records"];
    if (!records.IsArray()) {
        return Unexpected(Status{StatusCode::ParseError, "Index has no records array"});
    }

    std::vector<AssetRecord> loaded;
    loaded.reserve(records.Size());
    for (const serial::JsonValue& entry : records.AsArray()) {
        auto id = AssetId::Parse(entry["id"].AsString());
        if (id.IsError()) {
            return Unexpected(id.Error());
        }
        AssetRecord record;
        record.id = *id;
        record.path = AssetPath{entry["path"].AsString()};
        record.type = AssetTypeFromString(entry["type"].AsString("Unknown"));
        record.importer = std::string(entry["importer"].AsString());
        record.importerVersion = static_cast<u32>(entry["importerVersion"].AsInt(0));
        record.settingsHash = FromHex(entry["settingsHash"].AsString());
        record.sourceHash = FromHex(entry["sourceHash"].AsString());
        record.cookedSourceHash = FromHex(entry["cookedSourceHash"].AsString());
        record.cookedImportHash = FromHex(entry["cookedImportHash"].AsString());
        loaded.push_back(std::move(record));
    }

    records_ = std::move(loaded);
    RebuildIndices();
    return records_.size();
}

void AssetDatabase::RebuildIndices() {
    byId_.clear();
    byPath_.clear();
    byId_.reserve(records_.size());
    byPath_.reserve(records_.size());
    for (usize i = 0; i < records_.size(); ++i) {
        byId_.emplace(records_[i].id, i);
        byPath_.emplace(records_[i].path, i);
    }
}

} // namespace l3d::assets

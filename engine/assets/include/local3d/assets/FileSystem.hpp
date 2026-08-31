#pragma once
/// @file FileSystem.hpp
/// @brief The file access seam every part of the pipeline goes through.
///
/// Nothing in the asset system calls std::filesystem or stdio directly.  That
/// buys three things we need immediately and one we will need later:
///   * tests run against MemoryFileSystem, so they are fast, parallel safe and
///     leave no temporary directories behind;
///   * the cooker and the runtime read and write through the same interface, so
///     a cooked pack can be served from memory or from a mounted archive;
///   * errors are explicit (`Result`) instead of exceptions or silent defaults.
///
/// All paths are UTF-8, use '/' separators and are interpreted relative to the
/// file system's root.  Implementations are not required to be thread safe;
/// callers that share one across threads must synchronise.

#include "local3d/core/Common.hpp"
#include "local3d/core/Result.hpp"

#include <map>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace l3d::assets {

class IFileSystem {
public:
    virtual ~IFileSystem() = default;

    /// Reads a whole file.  `NotFound` when it does not exist, `IoError` when
    /// the read failed part way through.
    [[nodiscard]] virtual Result<std::vector<u8>> ReadFile(std::string_view path) = 0;

    /// Writes a whole file, creating parent directories as needed.
    [[nodiscard]] virtual OperationResult WriteFile(std::string_view path, ConstByteSpan data) = 0;

    [[nodiscard]] virtual bool Exists(std::string_view path) = 0;
    [[nodiscard]] virtual bool IsDirectory(std::string_view path) = 0;
    [[nodiscard]] virtual OperationResult CreateDirectories(std::string_view path) = 0;
    [[nodiscard]] virtual OperationResult Remove(std::string_view path) = 0;

    /// Immediate children of a directory, names only (no path prefix), in
    /// unspecified order.  Returns an empty list for a missing directory.
    [[nodiscard]] virtual Result<std::vector<std::string>> ListDirectory(std::string_view path) = 0;

    /// File size in bytes, or 0 when unavailable.
    [[nodiscard]] virtual u64 FileSize(std::string_view path) = 0;

    /// The path as the host understands it - for logging and for handing a path
    /// to a third party library that only speaks native paths.
    [[nodiscard]] virtual std::string ToNativePath(std::string_view path) const = 0;
};

/// Reads and writes real files under a root directory.
[[nodiscard]] std::unique_ptr<IFileSystem> CreateDiskFileSystem(std::string_view rootDirectory);

/// A file system backed by a map.  Used by tests and for serving cooked packs.
class MemoryFileSystem final : public IFileSystem {
public:
    [[nodiscard]] Result<std::vector<u8>> ReadFile(std::string_view path) override;
    [[nodiscard]] OperationResult WriteFile(std::string_view path, ConstByteSpan data) override;
    [[nodiscard]] bool Exists(std::string_view path) override;
    [[nodiscard]] bool IsDirectory(std::string_view path) override;
    [[nodiscard]] OperationResult CreateDirectories(std::string_view path) override;
    [[nodiscard]] OperationResult Remove(std::string_view path) override;
    [[nodiscard]] Result<std::vector<std::string>> ListDirectory(std::string_view path) override;
    [[nodiscard]] u64 FileSize(std::string_view path) override;
    [[nodiscard]] std::string ToNativePath(std::string_view path) const override;

    /// Total bytes held, for tests that assert on growth.
    [[nodiscard]] usize TotalBytes() const noexcept;
    [[nodiscard]] usize FileCount() const noexcept { return files_.size(); }

private:
    /// Files by normalised path.  A std::map keeps iteration deterministic,
    /// which matters for cook output stability.
    std::map<std::string, std::vector<u8>, std::less<>> files_;
    /// Explicitly created directories; parents of files are implied.
    std::set<std::string, std::less<>> directories_;
};

/// Normalises a path to the canonical asset form: '/' separators, no '.' or
/// '..' components, no leading or trailing slash.  Returns false when the path
/// tries to escape the root.
[[nodiscard]] bool NormalizePath(std::string_view raw, std::string& out);

/// Convenience: read a file as text.
[[nodiscard]] Result<std::string> ReadFileAsText(IFileSystem& fs, std::string_view path);

} // namespace l3d::assets

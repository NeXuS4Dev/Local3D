#include "local3d/assets/FileSystem.hpp"

#include <filesystem>
#include <fstream>
#include <system_error>

namespace l3d::assets {

bool NormalizePath(std::string_view raw, std::string& out) {
    out.clear();
    std::string component;
    for (usize i = 0; i <= raw.size(); ++i) {
        const bool atEnd = i == raw.size();
        const char c = atEnd ? '/' : raw[i];
        if (c == '/' || c == '\\') {
            if (component == "..") {
                out.clear();
                return false;
            }
            if (!component.empty() && component != ".") {
                if (!out.empty()) {
                    out += '/';
                }
                out += component;
            }
            component.clear();
        } else {
            component += c;
        }
    }
    return true;
}

Result<std::string> ReadFileAsText(IFileSystem& fs, std::string_view path) {
    auto bytes = fs.ReadFile(path);
    if (bytes.IsError()) {
        return Unexpected(bytes.Error());
    }
    return std::string(bytes->begin(), bytes->end());
}

// --- Disk ------------------------------------------------------------------

namespace {

class DiskFileSystem final : public IFileSystem {
public:
    explicit DiskFileSystem(std::string_view rootDirectory) : root_(std::string(rootDirectory)) {}

    [[nodiscard]] Result<std::vector<u8>> ReadFile(std::string_view path) override {
        std::filesystem::path native;
        if (!Resolve(path, native)) {
            return Unexpected(Status{StatusCode::InvalidArgument, "Path escapes the asset root"});
        }
        std::error_code ec;
        if (!std::filesystem::exists(native, ec) || std::filesystem::is_directory(native, ec)) {
            return Unexpected(Status{StatusCode::NotFound, "No such file"});
        }
        std::ifstream stream(native, std::ios::binary | std::ios::ate);
        if (!stream) {
            return Unexpected(Status{StatusCode::IoError, "Could not open file for reading"});
        }
        const auto size = stream.tellg();
        if (size < 0) {
            return Unexpected(Status{StatusCode::IoError, "Could not determine file size"});
        }
        stream.seekg(0, std::ios::beg);
        std::vector<u8> bytes(static_cast<usize>(size));
        if (size > 0 && !stream.read(reinterpret_cast<char*>(bytes.data()), size)) {
            return Unexpected(Status{StatusCode::IoError, "Read failed part way through"});
        }
        return bytes;
    }

    [[nodiscard]] OperationResult WriteFile(std::string_view path, ConstByteSpan data) override {
        std::filesystem::path native;
        if (!Resolve(path, native)) {
            return Unexpected(Status{StatusCode::InvalidArgument, "Path escapes the asset root"});
        }
        std::error_code ec;
        if (native.has_parent_path()) {
            std::filesystem::create_directories(native.parent_path(), ec);
            if (ec) {
                return Unexpected(Status{StatusCode::IoError, "Could not create directories"});
            }
        }
        std::ofstream stream(native, std::ios::binary | std::ios::trunc);
        if (!stream) {
            return Unexpected(Status{StatusCode::IoError, "Could not open file for writing"});
        }
        if (!data.empty()) {
            stream.write(reinterpret_cast<const char*>(data.data()),
                         static_cast<std::streamsize>(data.size()));
        }
        stream.flush();
        if (!stream) {
            return Unexpected(Status{StatusCode::IoError, "Write failed"});
        }
        return {};
    }

    [[nodiscard]] bool Exists(std::string_view path) override {
        std::filesystem::path native;
        std::error_code ec;
        return Resolve(path, native) && std::filesystem::exists(native, ec);
    }

    [[nodiscard]] bool IsDirectory(std::string_view path) override {
        std::filesystem::path native;
        std::error_code ec;
        return Resolve(path, native) && std::filesystem::is_directory(native, ec);
    }

    [[nodiscard]] OperationResult CreateDirectories(std::string_view path) override {
        std::filesystem::path native;
        if (!Resolve(path, native)) {
            return Unexpected(Status{StatusCode::InvalidArgument, "Path escapes the asset root"});
        }
        std::error_code ec;
        std::filesystem::create_directories(native, ec);
        if (ec) {
            return Unexpected(Status{StatusCode::IoError, "Could not create directories"});
        }
        return {};
    }

    [[nodiscard]] OperationResult Remove(std::string_view path) override {
        std::filesystem::path native;
        if (!Resolve(path, native)) {
            return Unexpected(Status{StatusCode::InvalidArgument, "Path escapes the asset root"});
        }
        std::error_code ec;
        // Idempotent: removing something that is already gone is not an error.
        std::filesystem::remove(native, ec);
        if (ec) {
            return Unexpected(Status{StatusCode::IoError, "Could not remove file"});
        }
        return {};
    }

    [[nodiscard]] Result<std::vector<std::string>> ListDirectory(std::string_view path) override {
        std::filesystem::path native;
        if (!Resolve(path, native)) {
            return Unexpected(Status{StatusCode::InvalidArgument, "Path escapes the asset root"});
        }
        std::vector<std::string> entries;
        std::error_code ec;
        if (!std::filesystem::is_directory(native, ec)) {
            return entries;
        }
        for (const std::filesystem::directory_entry& entry :
             std::filesystem::directory_iterator(native, ec)) {
            entries.push_back(entry.path().filename().string());
        }
        if (ec) {
            return Unexpected(Status{StatusCode::IoError, "Directory listing failed"});
        }
        return entries;
    }

    [[nodiscard]] u64 FileSize(std::string_view path) override {
        std::filesystem::path native;
        std::error_code ec;
        if (!Resolve(path, native)) {
            return 0;
        }
        const auto size = std::filesystem::file_size(native, ec);
        if (ec) {
            return 0;
        }
        return static_cast<u64>(size);
    }

    [[nodiscard]] std::string ToNativePath(std::string_view path) const override {
        std::string normalized;
        if (!NormalizePath(path, normalized)) {
            return {};
        }
        return (root_ / std::filesystem::path(normalized)).string();
    }

private:
    [[nodiscard]] bool Resolve(std::string_view path, std::filesystem::path& out) const {
        std::string normalized;
        if (!NormalizePath(path, normalized)) {
            return false;
        }
        out = root_ / std::filesystem::path(normalized);
        return true;
    }

    std::filesystem::path root_;
};

} // namespace

std::unique_ptr<IFileSystem> CreateDiskFileSystem(std::string_view rootDirectory) {
    return std::make_unique<DiskFileSystem>(rootDirectory);
}

// --- Memory ----------------------------------------------------------------

Result<std::vector<u8>> MemoryFileSystem::ReadFile(std::string_view path) {
    std::string key;
    if (!NormalizePath(path, key)) {
        return Unexpected(Status{StatusCode::InvalidArgument, "Path escapes the asset root"});
    }
    const auto found = files_.find(key);
    if (found == files_.end()) {
        return Unexpected(Status{StatusCode::NotFound, "No such file"});
    }
    return found->second;
}

OperationResult MemoryFileSystem::WriteFile(std::string_view path, ConstByteSpan data) {
    std::string key;
    if (!NormalizePath(path, key) || key.empty()) {
        return Unexpected(Status{StatusCode::InvalidArgument, "Path escapes the asset root"});
    }
    const auto* first = reinterpret_cast<const u8*>(data.data());
    files_[key].assign(first, first + data.size());
    return {};
}

bool MemoryFileSystem::Exists(std::string_view path) {
    std::string key;
    return NormalizePath(path, key) && (files_.contains(key) || IsDirectory(path));
}

bool MemoryFileSystem::IsDirectory(std::string_view path) {
    std::string key;
    if (!NormalizePath(path, key)) {
        return false;
    }
    if (key.empty()) {
        return true; // The root always exists.
    }
    if (directories_.contains(key)) {
        return true;
    }
    const std::string prefix = key + "/";
    for (const auto& [filePath, unused] : files_) {
        if (filePath.starts_with(prefix)) {
            return true;
        }
    }
    return false;
}

OperationResult MemoryFileSystem::CreateDirectories(std::string_view path) {
    std::string key;
    if (!NormalizePath(path, key)) {
        return Unexpected(Status{StatusCode::InvalidArgument, "Path escapes the asset root"});
    }
    if (!key.empty()) {
        directories_.insert(key);
        // Record the parents too so a later IsDirectory("a") sees "a/b".
        for (usize slash = key.find('/'); slash != std::string::npos;
             slash = key.find('/', slash + 1)) {
            directories_.insert(key.substr(0, slash));
        }
    }
    return {};
}

OperationResult MemoryFileSystem::Remove(std::string_view path) {
    std::string key;
    if (!NormalizePath(path, key)) {
        return Unexpected(Status{StatusCode::InvalidArgument, "Path escapes the asset root"});
    }
    files_.erase(key);
    directories_.erase(key);
    return {};
}

Result<std::vector<std::string>> MemoryFileSystem::ListDirectory(std::string_view path) {
    std::string key;
    if (!NormalizePath(path, key)) {
        return Unexpected(Status{StatusCode::InvalidArgument, "Path escapes the asset root"});
    }
    const std::string prefix = key.empty() ? std::string{} : key + "/";
    std::set<std::string, std::less<>> names;
    for (const auto& [filePath, unused] : files_) {
        if (!filePath.starts_with(prefix)) {
            continue;
        }
        const std::string_view remainder(filePath.data() + prefix.size(),
                                        filePath.size() - prefix.size());
        const usize slash = remainder.find('/');
        names.insert(std::string(remainder.substr(0, slash)));
    }
    for (const std::string& dir : directories_) {
        if (!dir.starts_with(prefix)) {
            continue;
        }
        const std::string_view remainder(dir.data() + prefix.size(), dir.size() - prefix.size());
        const usize slash = remainder.find('/');
        if (!remainder.empty()) {
            names.insert(std::string(remainder.substr(0, slash)));
        }
    }
    return std::vector<std::string>(names.begin(), names.end());
}

u64 MemoryFileSystem::FileSize(std::string_view path) {
    std::string key;
    if (!NormalizePath(path, key)) {
        return 0;
    }
    const auto found = files_.find(key);
    return found == files_.end() ? 0 : static_cast<u64>(found->second.size());
}

std::string MemoryFileSystem::ToNativePath(std::string_view path) const {
    std::string key;
    return NormalizePath(path, key) ? key : std::string{};
}

usize MemoryFileSystem::TotalBytes() const noexcept {
    usize total = 0;
    for (const auto& [unused, bytes] : files_) {
        total += bytes.size();
    }
    return total;
}

} // namespace l3d::assets

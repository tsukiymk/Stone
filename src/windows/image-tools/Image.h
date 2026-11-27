#pragma once

#include <windows.h>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <ostream>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace stone::image {
namespace fs = std::filesystem;
using Bytes = std::vector<uint8_t>;
using View = std::span<const uint8_t>;
void Require(bool condition, const std::string& message);
std::string Utf8(const fs::path& path);
fs::path Path(std::string_view text);
std::string Text(View bytes);
bool SafeName(std::string_view name);

template <class T> T Little(View bytes, size_t at)
{
    Require(at <= bytes.size() && sizeof(T) <= bytes.size() - at, "Truncated image metadata");
    T value{};
    std::memcpy(&value, bytes.data() + at, sizeof(value));
    return value;
}

inline uint16_t U16(View bytes, size_t at)
{
    return Little<uint16_t>(bytes, at);
}

inline uint32_t U32(View bytes, size_t at)
{
    return Little<uint32_t>(bytes, at);
}

inline int32_t I32(View bytes, size_t at)
{
    return Little<int32_t>(bytes, at);
}

inline int64_t I64(View bytes, size_t at)
{
    return Little<int64_t>(bytes, at);
}

class Handle {
public:
    HANDLE value;

    explicit Handle(HANDLE handle = nullptr) : value(handle == INVALID_HANDLE_VALUE ? nullptr : handle)
    {
    }

    ~Handle()
    {
        if (value)
            CloseHandle(value);
    }

    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
};

class Image {
public:
    using Reader = std::function<void(int64_t, std::span<uint8_t>)>;
    Reader reader;
    int64_t size = 0;
    Image() = default;

    Image(Reader read, int64_t length) : reader(std::move(read)), size(length)
    {
    }

    void Range(int64_t offset, int64_t length) const;
    void Read(int64_t offset, std::span<uint8_t> bytes) const;
    Bytes Read(int64_t offset, size_t length) const;
    Image Slice(int64_t offset, int64_t length) const;
};

Image OpenFile(const fs::path& path);
Image OpenSystem(Image image);
std::optional<Image> OpenImagePartition(Image image, std::string_view name);

struct Entry {
    std::string name;
    int type;
};

class Partition {
public:
    static constexpr int Directory = 0x4000, Regular = 0x8000, Symlink = 0xa000;
    virtual ~Partition() = default;
    virtual std::vector<Entry> List(const std::string& directory) const = 0;
    virtual bool Copy(const std::string& directory, const std::string& name, std::ostream& output) const = 0;
    virtual std::string Link(const std::string& directory, const std::string& name) const = 0;
    std::vector<std::string> Files(const std::string& directory) const;
    Bytes Read(const std::string& directory, const std::string& name) const;
};

std::shared_ptr<Partition> OpenPartition(Image image);
std::shared_ptr<Partition> OpenExt4(Image image);
std::shared_ptr<Partition> OpenErofs(Image image);
void DirectorySafe(const fs::path& path);
void Atomic(const fs::path& path, const std::function<void(std::ostream&)>& write);
void ZipEntry(const fs::path& archive, std::string_view name, const fs::path& target);
using Properties = std::map<std::string, std::string>;
std::vector<std::string> BootClassPath(View fragment, const std::string& prefix, const Properties& properties);
std::string ApexName(View manifest);
bool ModuleName(const std::string& name);
std::vector<std::string> ElfDependencies(View file);
fs::path Prepare(const fs::path& image, const fs::path& cache, const std::vector<std::string>& extras);
}

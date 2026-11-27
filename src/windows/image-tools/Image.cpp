#include "Image.h"
#include "../host/WindowsPath.h"
#include <climits>
#include <fstream>
#include <optional>
#include <sstream>

namespace stone::image {
void Require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

std::string Utf8(const fs::path& path)
{
    auto text = path.u8string();
    return {reinterpret_cast<const char*>(text.data()), text.size()};
}

fs::path Path(std::string_view text)
{
    Require(text.size() <= INT_MAX, "Image path is too long");
    if (text.empty())
        return {};
    int size =
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
    Require(size > 0, "Invalid image UTF-8");
    std::wstring result(size, 0);
    Require(MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
                                result.data(), size) == size,
            "Invalid image UTF-8");
    return result;
}

std::string Text(View bytes)
{
    std::string text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    Path(text);
    return text;
}

bool SafeName(std::string_view name)
{
    if (name.empty() || name == "." || name == ".." || name.back() == '.' || name.back() == ' ')
        return false;
    for (unsigned char c : name)
        if (c < 32 || std::string_view("\\/:<>|\"?*").find(c) != std::string_view::npos)
            return false;
    auto wide = Path(name).wstring();
    wide.resize(wide.find(L'.') == std::wstring::npos ? wide.size() : wide.find(L'.'));
    for (auto& c : wide)
        if (c >= L'a' && c <= L'z')
            c -= L'a' - L'A';
    return wide != L"CON" && wide != L"PRN" && wide != L"AUX" && wide != L"NUL" &&
           !(wide.size() == 4 && (wide.starts_with(L"COM") || wide.starts_with(L"LPT")) &&
             ((wide[3] >= L'1' && wide[3] <= L'9') || wide[3] == L'¹' || wide[3] == L'²' || wide[3] == L'³'));
}

void Image::Range(int64_t offset, int64_t length) const
{
    Require(offset >= 0 && length >= 0 && length <= size && offset <= size - length, "Read outside image or partition");
}

void Image::Read(int64_t offset, std::span<uint8_t> bytes) const
{
    Range(offset, bytes.size());
    reader(offset, bytes);
}

Bytes Image::Read(int64_t offset, size_t length) const
{
    Range(offset, length);
    Bytes bytes(length);
    Read(offset, bytes);
    return bytes;
}

Image Image::Slice(int64_t offset, int64_t length) const
{
    Range(offset, length);
    return {[source = *this, offset](int64_t at, std::span<uint8_t> out) {
                source.Read(offset + at, out);
            },
            length};
}

Image OpenFile(const fs::path& path)
{
    struct Mapping {
        Handle file, mapping;
        const uint8_t* data = nullptr;

        explicit Mapping(const fs::path& path)
            : file(CreateFileW(stone::windows::ExtendedWindowsPath(path).c_str(), GENERIC_READ, FILE_SHARE_READ,
                               nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr))
        {
        }

        ~Mapping()
        {
            if (data)
                UnmapViewOfFile(data);
        }
    };

    auto source = std::make_shared<Mapping>(path);
    LARGE_INTEGER size{};
    Require(source->file.value && GetFileSizeEx(source->file.value, &size) && size.QuadPart > 0 &&
                size.QuadPart <= (1ll << 40),
            "Could not open image: " + Utf8(path));
    source->mapping.value = CreateFileMappingW(source->file.value, nullptr, PAGE_READONLY, 0, 0, nullptr);
    Require(source->mapping.value != nullptr, "Could not map image");
    source->data = static_cast<const uint8_t*>(MapViewOfFile(source->mapping.value, FILE_MAP_READ, 0, 0, 0));
    Require(source->data != nullptr, "Could not map image view");
    return {[source](int64_t offset, std::span<uint8_t> output) {
                std::memcpy(output.data(), source->data + offset, output.size());
            },
            size.QuadPart};
}

namespace {
struct Extent {
    int64_t start, end, source;
    uint32_t fill;
};

class Ranges {
    Image source_;
    std::vector<Extent> ranges_;
    int64_t size_ = 0;

public:
    explicit Ranges(Image source) : source_(std::move(source))
    {
    }

    void Add(int64_t length, int64_t source, uint32_t fill = 0)
    {
        Require(length > 0 && length <= INT64_MAX - size_ && ranges_.size() < 1'000'000,
                "Invalid image extent length/count");
        if (source >= 0)
            source_.Range(source, length);
        ranges_.push_back({size_, size_ + length, source, fill});
        size_ += length;
    }

    Image View() const
    {
        return {[ranges = ranges_, source = source_](int64_t offset, std::span<uint8_t> output) {
                    while (!output.empty()) {
                        auto found =
                            std::upper_bound(ranges.begin(), ranges.end(), offset, [](auto value, const Extent& range) {
                                return value < range.start;
                            });
                        Require(found != ranges.begin(), "Missing image extent");
                        const auto& range = *--found;
                        Require(offset < range.end, "Invalid image extent coverage");
                        size_t count = static_cast<size_t>(std::min<int64_t>(output.size(), range.end - offset));
                        int64_t within = offset - range.start;
                        if (range.source >= 0)
                            source.Read(range.source + within, output.first(count));
                        else
                            for (size_t i = 0; i < count; i++)
                                output[i] = static_cast<uint8_t>(range.fill >> (8 * ((within + i) % 4)));
                        output = output.subspan(count);
                        offset += count;
                    }
                },
                size_};
    }
};

int64_t Sectors(int64_t value)
{
    Require(value >= 0 && value <= INT64_MAX / 512, "Invalid image sector offset");
    return value * 512;
}

bool Super(const Image& image)
{
    return image.size >= 12288 &&
           (U32(image.Read(4096, 4), 0) == 0x616c4467 || U32(image.Read(8192, 4), 0) == 0x616c4467);
}

std::optional<Image> Metadata(const Image& image, int64_t offset, int capacity, std::string_view requested)
{
    auto header = image.Read(offset, 128);
    Require(U32(header, 0) == 0x414c5030 && U16(header, 4) == 10 && U16(header, 6) <= 2, "Unsupported super metadata");
    int headerSize = I32(header, 8), tableSize = I32(header, 44);
    Require((headerSize == 128 || headerSize == 256) && tableSize >= 0 && tableSize <= capacity - headerSize,
            "Invalid super metadata size");
    auto tables = image.Read(offset + headerSize, tableSize);
    auto count = [&](int descriptor, int stride) {
        int start = I32(header, descriptor), size = I32(header, descriptor + 4);
        Require(I32(header, descriptor + 8) == stride && start >= 0 && size >= 0 &&
                    start + int64_t(size) * stride <= tableSize,
                "Invalid super metadata table");
        return size;
    };
    int partitions = count(80, 52), extents = count(92, 24), partition = -1;
    for (int i = 0; i < partitions; i++) {
        int row = I32(header, 80) + i * 52;
        std::string name(reinterpret_cast<const char*>(tables.data() + row), 36);
        name.resize(name.find('\0') == std::string::npos ? 36 : name.find('\0'));
        if ((I32(tables, row + 36) & 8) || !I32(tables, row + 44))
            continue;
        if (name == requested) {
            partition = row;
            break;
        }
        if (name == std::string(requested) + "_a")
            partition = row;
    }
    if (partition < 0)
        return {};
    int first = I32(tables, partition + 40), size = I32(tables, partition + 44);
    Require(first >= 0 && size > 0 && size <= extents - first, "Invalid logical extents");
    Ranges ranges(image);
    for (int i = 0; i < size; i++) {
        int row = I32(header, 92) + (first + i) * 24;
        int64_t length = Sectors(I64(tables, row));
        int type = I32(tables, row + 8);
        if (type == 1)
            ranges.Add(length, -1);
        else {
            Require(type == 0 && I32(tables, row + 20) == 0, "Unsupported logical extent");
            ranges.Add(length, Sectors(I64(tables, row + 12)));
        }
    }
    return ranges.View();
}

std::optional<Image> Logical(const Image& image, std::string_view name)
{
    auto geometry = image.Read(4096, 52);
    if (U32(geometry, 0) != 0x616c4467)
        geometry = image.Read(8192, 52);
    int capacity = I32(geometry, 40), slots = I32(geometry, 44);
    Require(capacity >= 128 && capacity <= 4 * 1024 * 1024 && slots > 0 && slots <= 3, "Invalid super geometry");
    try {
        if (auto result = Metadata(image, 12288, capacity, name))
            return result;
    }
    catch (const std::runtime_error&) {
        return Metadata(image, 12288 + int64_t(capacity) * slots, capacity, name);
    }
    return {};
}

Image Unsparse(const Image& image)
{
    auto header = image.Read(0, 28);
    int64_t position = U16(header, 8), block = U32(header, 12), blocks = U32(header, 16), chunks = U32(header, 20);
    int chunkHeader = U16(header, 10);
    Require(U16(header, 4) == 1 && position >= 28 && chunkHeader >= 12 && block > 0 && block % 4 == 0 &&
                blocks <= INT64_MAX / block && chunks <= (image.size - position) / chunkHeader && chunks <= 1'000'000,
            "Invalid sparse image header");
    Ranges ranges(image);
    int64_t remaining = blocks * block;
    for (int64_t i = 0; i < chunks; i++) {
        auto chunk = image.Read(position, 12);
        int64_t size = U32(chunk, 8), length = int64_t(U32(chunk, 4)) * block, data = position + chunkHeader;
        Require(length <= remaining && size >= chunkHeader, "Invalid sparse chunk size");
        image.Range(position, size);
        int64_t payload = size - chunkHeader;
        switch (U16(chunk, 0)) {
        case 0xcac1:
            Require(length == payload && length > 0, "Invalid raw sparse chunk");
            ranges.Add(length, data);
            break;
        case 0xcac2:
            Require(payload == 4 && length > 0, "Invalid fill sparse chunk");
            ranges.Add(length, -1, U32(image.Read(data, 4), 0));
            break;
        case 0xcac3:
            Require(payload == 0 && length > 0, "Invalid empty sparse chunk");
            ranges.Add(length, -1);
            break;
        case 0xcac4:
            Require(payload == 4 && length == 0, "Invalid CRC sparse chunk");
            break;
        default:
            throw std::runtime_error("Unsupported Android sparse chunk");
        }
        remaining -= length;
        position += size;
    }
    Require(remaining == 0, "Incomplete Android sparse image");
    return ranges.View();
}
}

std::optional<Image> OpenImagePartition(Image image, std::string_view name)
{
    Require(name == "system" || name == "product" || name == "system_ext", "Unsupported image partition name");
    const auto requested = Path(name).wstring(), slotted = requested + L"_a";
    if (U32(image.Read(0, 4), 0) == 0xed26ff3a)
        image = Unsparse(image);
    if (image.size >= 1024) {
        auto header = image.Read(512, 512);
        if (I64(header, 0) == 0x5452415020494645ll) {
            int64_t table = Sectors(I64(header, 72));
            int count = I32(header, 80), stride = I32(header, 84);
            Require(count > 0 && count <= 4096 && stride >= 128 && stride <= 4096, "Invalid GPT table");
            image.Range(table, int64_t(count) * stride);
            std::optional<Image> super, slot;
            for (int i = 0; i < count; i++) {
                auto row = image.Read(table + int64_t(i) * stride, 128);
                if (!I64(row, 0) && !I64(row, 8))
                    continue;
                std::wstring label(36, 0);
                std::memcpy(label.data(), row.data() + 56, 72);
                label.resize(label.find(L'\0') == std::wstring::npos ? 36 : label.find(L'\0'));
                if (label != requested && label != L"super" && label != slotted)
                    continue;
                int64_t first = Sectors(I64(row, 32)), last = Sectors(I64(row, 40));
                Require(last >= first, "Invalid GPT partition bounds");
                image.Range(last, 512);
                Image part = image.Slice(first, last - first + 512);
                if (label == requested) {
                    if (!Super(part))
                        return part;
                    return Logical(part, name);
                }
                if (label == L"super")
                    super = part;
                else
                    slot = part;
            }
            if (super)
                if (auto found = Logical(*super, name))
                    return *found;
            if (!slot)
                return {};
            image = *slot;
            if (!Super(image))
                return image;
        }
    }
    if (!Super(image))
        return name == "system" ? std::optional(image) : std::nullopt;
    return Logical(image, name);
}

Image OpenSystem(Image image)
{
    auto system = OpenImagePartition(std::move(image), "system");
    Require(system.has_value(), "Android image has no system partition");
    return *system;
}

std::vector<std::string> Partition::Files(const std::string& directory) const
{
    std::vector<std::string> result;
    for (const auto& entry : List(directory))
        if (entry.type == Regular)
            result.push_back(entry.name);
    return result;
}

Bytes Partition::Read(const std::string& directory, const std::string& name) const
{
    auto files = Files(directory);
    if (std::find(files.begin(), files.end(), name) == files.end())
        return {};
    std::ostringstream output(std::ios::binary);
    Require(Copy(directory, name, output), "Missing image file: " + name);
    auto value = output.str();
    return {value.begin(), value.end()};
}

std::shared_ptr<Partition> OpenPartition(Image image)
{
    return image.size >= 1028 && U32(image.Read(1024, 4), 0) == 0xe0f5e1e2 ? OpenErofs(std::move(image))
                                                                           : OpenExt4(std::move(image));
}
}

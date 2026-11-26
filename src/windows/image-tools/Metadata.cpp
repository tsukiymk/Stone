#include "Image.h"
#include <charconv>
#include <sstream>

namespace stone::image {
namespace {
class Proto {
    View bytes_;
    size_t cursor_ = 0;

public:
    explicit Proto(View bytes) : bytes_(bytes)
    {
    }

    bool More() const
    {
        return cursor_ < bytes_.size();
    }

    uint64_t Number()
    {
        uint64_t value = 0;
        for (int shift = 0; shift < 64; shift += 7) {
            Require(More(), "Truncated protobuf integer");
            int next = bytes_[cursor_++];
            Require(shift != 63 || !(next & 254), "Protobuf integer overflow");
            value |= uint64_t(next & 127) << shift;
            if (!(next & 128))
                return value;
        }
        throw std::runtime_error("Invalid protobuf integer");
    }

    uint32_t Tag()
    {
        auto value = Number();
        Require(value >= 8 && value <= UINT32_MAX, "Invalid protobuf tag");
        return static_cast<uint32_t>(value);
    }

    View Message()
    {
        auto size = Number();
        Require(size <= bytes_.size() - cursor_, "Truncated protobuf message");
        auto result = bytes_.subspan(cursor_, static_cast<size_t>(size));
        cursor_ += static_cast<size_t>(size);
        return result;
    }

    std::string String()
    {
        return Text(Message());
    }

    void Skip(uint32_t tag)
    {
        size_t size;
        switch (tag & 7) {
        case 0:
            Number();
            return;
        case 1:
            size = 8;
            break;
        case 2:
            Message();
            return;
        case 5:
            size = 4;
            break;
        default:
            throw std::runtime_error("Unsupported protobuf wire type");
        }
        Require(size <= bytes_.size() - cursor_, "Truncated protobuf field");
        cursor_ += size;
    }
};

int Sdk(const std::string& text)
{
    int value = 0;
    auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    Require(result.ec == std::errc{} && result.ptr == text.data() + text.size() && value > 0,
            "Invalid image/classpath SDK version: " + text);
    return value;
}

bool Allowed(const std::string& minimum, const std::string& maximum, const Properties& properties)
{
    auto property = [&](const std::string& name, const std::string& fallback) {
        if (auto found = properties.find("ro.build.version." + name); found != properties.end())
            return found->second;
        if (auto found = properties.find("ro.system.build.version." + name); found != properties.end())
            return found->second;
        return fallback;
    };
    int sdk = Sdk(property("sdk", "0"));
    auto codename = property("codename", "REL");
    bool release = codename == "REL";
    std::set<std::string> known;
    for (const auto* key : {"known_codenames", "all_codenames"}) {
        std::istringstream values(property(key, ""));
        for (std::string value; std::getline(values, value, ',');)
            known.insert(value);
    }
    auto uppercase = [](char c) {
        return c >= 'A' && c <= 'Z';
    };
    if (!minimum.empty()) {
        if (uppercase(minimum[0])) {
            if (release || !known.contains(minimum))
                return false;
        }
        else if (sdk < Sdk(minimum))
            return false;
    }
    if (!maximum.empty()) {
        if (uppercase(maximum[0])) {
            if (!release && known.contains(maximum) && codename != maximum)
                return false;
        }
        else if (release ? sdk > Sdk(maximum) : sdk >= Sdk(maximum))
            return false;
    }
    return true;
}
}

bool ModuleName(const std::string& name)
{
    bool dot = false, part = false;
    for (char c : name) {
        if (c == '.') {
            if (!part)
                return false;
            dot = true;
            part = false;
        }
        else {
            if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'))
                return false;
            part = true;
        }
    }
    return dot && part;
}

std::string ApexName(View manifest)
{
    Proto bytes(manifest);
    while (bytes.More()) {
        auto tag = bytes.Tag();
        if (tag != 10) {
            bytes.Skip(tag);
            continue;
        }
        auto name = bytes.String();
        Require(ModuleName(name), "Invalid APEX name");
        return name;
    }
    throw std::runtime_error("APEX manifest has no name");
}

std::vector<std::string> BootClassPath(View fragment, const std::string& prefix, const Properties& properties)
{
    Require(fragment.size() <= 1024 * 1024, "Classpath fragment is too large");
    Proto bytes(fragment);
    std::vector<std::string> paths;
    while (bytes.More()) {
        auto tag = bytes.Tag();
        if (tag != 10) {
            bytes.Skip(tag);
            continue;
        }
        Proto jar(bytes.Message());
        std::string path, minimum, maximum;
        uint64_t kind = 0;
        while (jar.More()) {
            auto field = jar.Tag();
            switch (field) {
            case 10:
                path = jar.String();
                break;
            case 16:
                kind = jar.Number();
                break;
            case 26:
                minimum = jar.String();
                break;
            case 34:
                maximum = jar.String();
                break;
            default:
                jar.Skip(field);
            }
        }
        if (kind != 1 || !Allowed(minimum, maximum, properties))
            continue;
        bool valid = path.starts_with(prefix) && (path.starts_with("/system/") || path.starts_with("/apex/")) &&
                     path.ends_with(".jar") && path.find("//") == std::string::npos;
        for (unsigned char c : path)
            valid = valid && ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                              std::string_view("_./+@-").find(c) != std::string_view::npos);
        std::istringstream parts(path);
        for (std::string part; std::getline(parts, part, '/');)
            if (part == "." || part == "..")
                valid = false;
        Require(valid, "Invalid boot class path: " + path);
        paths.push_back(path);
    }
    return paths;
}

std::vector<std::string> ElfDependencies(View file)
{
    auto at = [&](int64_t offset, int64_t size) {
        Require(offset >= 0 && size >= 0 && uint64_t(offset) <= file.size() && uint64_t(size) <= file.size() - offset,
                "ELF dependency metadata is outside its file");
        return static_cast<size_t>(offset);
    };
    at(0, 64);
    Require(file[0] == 0x7f && file[1] == 'E' && file[2] == 'L' && file[3] == 'F' && file[4] == 2 && file[5] == 1,
            "ART dependencies require little-endian ELF64");
    int64_t headers = I64(file, 32);
    int size = U16(file, 54), count = U16(file, 56);
    Require(size == 56, "Invalid ELF64 program header size");
    at(headers, int64_t(size) * count);

    struct Segment {
        int64_t address, offset, size;
    };

    std::vector<Segment> loads;
    int64_t dynamic = 0, dynamicSize = 0;
    for (int i = 0; i < count; i++) {
        auto header = at(headers + int64_t(i) * size, size);
        auto type = U32(file, header);
        int64_t offset = I64(file, header + 8), address = I64(file, header + 16), length = I64(file, header + 32);
        if (type == 1)
            loads.push_back({address, offset, length});
        if (type == 2) {
            dynamic = offset;
            dynamicSize = length;
        }
    }
    at(dynamic, dynamicSize);
    Require(dynamicSize % 16 == 0, "Invalid ELF64 dynamic table size");
    int64_t strings = 0, stringSize = 0;
    std::vector<int64_t> needed;
    for (int64_t offset = 0; offset < dynamicSize; offset += 16) {
        auto entry = at(dynamic + offset, 16);
        int64_t tag = I64(file, entry), value = I64(file, entry + 8);
        if (!tag)
            break;
        if (tag == 1)
            needed.push_back(value);
        if (tag == 5)
            strings = value;
        if (tag == 10)
            stringSize = value;
    }
    if (needed.empty())
        return {};
    int64_t stringOffset = -1;
    for (const auto& load : loads)
        if (load.address >= 0 && strings >= load.address && strings - load.address < load.size && stringSize > 0 &&
            stringSize <= load.size - (strings - load.address)) {
            Require(load.offset >= 0 && load.offset <= INT64_MAX - (strings - load.address),
                    "ELF string table overflow");
            stringOffset = load.offset + strings - load.address;
            break;
        }
    size_t start = at(stringOffset, stringSize);
    std::vector<std::string> result;
    for (auto name : needed) {
        Require(name >= 0 && name < stringSize, "Invalid ELF64 dependency name offset");
        size_t first = start + static_cast<size_t>(name), end = first, limit = start + static_cast<size_t>(stringSize);
        while (end < limit && file[end])
            end++;
        Require(end < limit, "Unterminated ELF64 dependency name");
        result.push_back(Text(file.subspan(first, end - first)));
    }
    return result;
}
}

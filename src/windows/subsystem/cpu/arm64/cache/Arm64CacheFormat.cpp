#include "cpu/arm64/cache/Arm64CacheFormat.h"
#include <algorithm>
#include <stdexcept>

namespace stone::windows::arm64_cache {
void Check(bool good)
{
    if (!good)
        throw std::runtime_error("invalid ARM64 native cache");
}

size_t Bytes(const Arm64BlockArtifact& a)
{
    return 32 + a.source.size() + a.code.size() + a.relocations.size() * 4;
}

struct Writer {
    std::vector<uint8_t> data;

    void U32(uint32_t value)
    {
        for (unsigned i = 0; i < 4; ++i)
            data.push_back(static_cast<uint8_t>(value >> (8 * i)));
    }

    void U64(uint64_t value)
    {
        for (unsigned i = 0; i < 8; ++i)
            data.push_back(static_cast<uint8_t>(value >> (8 * i)));
    }

    void Data(std::span<const uint8_t> bytes)
    {
        data.insert(data.end(), bytes.begin(), bytes.end());
    }
};

struct Reader {
    std::span<const uint8_t> data;

    std::span<const uint8_t> Take(size_t size)
    {
        Check(size <= data.size());
        auto value = data.first(size);
        data = data.subspan(size);
        return value;
    }

    uint32_t U32()
    {
        auto bytes = Take(4);
        uint32_t value = 0;
        for (unsigned i = 0; i < 4; ++i)
            value |= uint32_t{bytes[i]} << (8 * i);
        return value;
    }

    uint64_t U64()
    {
        auto bytes = Take(8);
        uint64_t value = 0;
        for (unsigned i = 0; i < 8; ++i)
            value |= uint64_t{bytes[i]} << (8 * i);
        return value;
    }
};

void Validate(const Arm64BlockArtifact& a, uint32_t minimum)
{
    Check(a.instructions && a.instructions <= 32 && !(a.flags & ~15u));
    Check(a.source.size() >= a.instructions * 4 && a.source.size() <= MaximumSource && !(a.source.size() & 3));
    Check(a.instructions >= minimum || (a.flags & 8));
    Check(a.code.size() >= 6 && a.code.size() <= MaximumCode && a.relocations.size() <= 256);
    uint32_t end = 0;
    for (auto offset : a.relocations) {
        Check(offset >= 2 && offset >= end && offset <= a.code.size() && a.code.size() - offset >= 8);
        Check(a.code[offset - 2] == 0x48 && a.code[offset - 1] == 0xb8); // movabs rax, address.
        end = offset + 8;
    }
}

std::shared_ptr<Bundle> Decode(std::span<const uint8_t> plain, const Digest& key)
{
    Reader reader{plain};
    Check(reader.U32() == 0x43425453 && reader.U32() == Format);
    const auto identity = reader.Take(key.size());
    Check(std::equal(identity.begin(), identity.end(), key.begin()));
    const auto count = reader.U32();
    Check(count <= MaximumBlocks);
    auto result = std::make_shared<Bundle>();
    result->key = key;
    result->loadAttempted = true;
    for (uint32_t i = 0; i < count; ++i) {
        const auto pc = reader.U64();
        const auto minimum = reader.U32();
        Check(!(pc & 3) && minimum && minimum <= 32);
        Arm64BlockArtifact a;
        a.instructions = reader.U32();
        a.flags = reader.U32();
        const auto source = reader.U32(), code = reader.U32(), relocations = reader.U32();
        Check(source <= MaximumSource && code <= MaximumCode && relocations <= 256);
        auto bytes = reader.Take(source);
        a.source.assign(bytes.begin(), bytes.end());
        bytes = reader.Take(code);
        a.code.assign(bytes.begin(), bytes.end());
        for (uint32_t j = 0; j < relocations; ++j)
            a.relocations.push_back(reader.U32());
        Validate(a, minimum);
        result->bytes += Bytes(a);
        Check(result->bytes <= MaximumImage);
        Check(result->blocks.emplace(Key{pc, minimum}, Saved{std::move(a), true}).second);
    }
    Check(reader.data.empty());
    return result;
}

std::vector<uint8_t> Encode(const Bundle& bundle)
{
    Writer writer;
    writer.U32(0x43425453);
    writer.U32(Format);
    writer.Data(bundle.key);
    writer.U32(static_cast<uint32_t>(bundle.blocks.size()));
    for (const auto& [key, saved] : bundle.blocks) {
        const auto& a = saved.artifact;
        Validate(a, key.second);
        writer.U64(key.first);
        writer.U32(key.second);
        writer.U32(a.instructions);
        writer.U32(a.flags);
        writer.U32(static_cast<uint32_t>(a.source.size()));
        writer.U32(static_cast<uint32_t>(a.code.size()));
        writer.U32(static_cast<uint32_t>(a.relocations.size()));
        writer.Data(a.source);
        writer.Data(a.code);
        for (auto offset : a.relocations)
            writer.U32(offset);
    }
    return std::move(writer.data);
}
}

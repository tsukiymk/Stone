// SPDX-License-Identifier: Apache-2.0
// Compact index lookup adapted from erofs-utils lib/zmap.c.
// Copyright (C) 2018-2019 HUAWEI, Inc.
// Created by Gao Xiang and modified by Huang Jianan in erofs-utils.
#include "Image.h"
#include <optional>
#include <sstream>

namespace stone::image {
namespace {
class Erofs final : public Partition {
    static constexpr int Nonhead = 2, BlockCountFlag = 1 << 11;
    Image image_;
    int block_;
    int64_t metadata_, root_;
    bool zeroPadded_;

    struct Inode {
        int layout, type;
        int64_t size, data, inlined;
    };

    struct Cluster {
        int index, type;
        int64_t logical, physical;
    };

    Inode Node(int64_t nid) const
    {
        Require(nid >= 0 && metadata_ <= image_.size && nid <= (image_.size - metadata_) / 32,
                "Invalid EROFS inode number");
        int64_t offset = metadata_ + nid * 32;
        auto bytes = image_.Read(offset, 32);
        int format = U16(bytes, 0), attrs = U16(bytes, 2);
        Require((format & ~15) == 0, "Unsupported EROFS inode format");
        bool extended = (format & 1) != 0;
        return {format >> 1, U16(bytes, 4) & 0xf000, extended ? I64(bytes, 8) : U32(bytes, 8),
                int64_t(U32(bytes, 16)) * block_,
                offset + (extended ? 64 : 32) + (attrs == 0 ? 0 : 12ll + 4ll * (attrs - 1))};
    }

    void Flat(const Inode& node, int64_t offset, std::span<uint8_t> output) const
    {
        Require(node.layout == 0 || node.layout == 2, "Unsupported EROFS flat inode layout");
        int64_t flat = node.layout == 2 ? node.size / block_ * block_ : node.size;
        size_t count = static_cast<size_t>(std::min<int64_t>(output.size(), std::max<int64_t>(0, flat - offset)));
        if (count) {
            image_.Read(node.data + offset, output.first(count));
            output = output.subspan(count);
            offset += count;
        }
        if (!output.empty()) {
            int64_t tail = node.inlined + offset - flat;
            Require(tail >= 0 && tail % block_ + output.size() <= block_, "Invalid EROFS inline data");
            image_.Read(tail, output);
        }
    }

    int64_t Visit(const Inode& node, const std::function<bool(const std::string&, int64_t)>& visitor) const
    {
        Require(node.type == Directory && node.size > 0 && node.size <= 64ll * 1024 * 1024, "Invalid EROFS directory");
        for (int64_t offset = 0; offset < node.size; offset += block_) {
            Bytes data(static_cast<size_t>(std::min<int64_t>(block_, node.size - offset)));
            Flat(node, offset, data);
            Require(data.size() >= 12, "Truncated EROFS directory");
            int first = U16(data, 8);
            Require(first >= 12 && first % 12 == 0 && first < data.size(), "Invalid EROFS directory table");
            for (int entry = 0, start = first; entry < first; entry += 12) {
                bool last = entry + 12 == first;
                int end = last ? static_cast<int>(data.size()) : U16(data, entry + 20);
                Require(start < end && end <= data.size(), "Invalid EROFS directory name");
                int nameEnd = end;
                if (last) {
                    nameEnd = start;
                    while (nameEnd < end && data[nameEnd])
                        nameEnd++;
                }
                auto name = Text(View(data).subspan(start, nameEnd - start));
                int64_t number = I64(data, entry);
                if (name != "." && name != ".." && visitor(name, number))
                    return number;
                start = end;
            }
        }
        return -1;
    }

    int64_t Child(const Inode& parent, const std::string& name) const
    {
        return Visit(parent, [&](const auto& entry, auto) {
            return name == entry;
        });
    }

    int64_t Under(int64_t nid, const std::string& path) const
    {
        std::istringstream parts(path);
        for (std::string part; std::getline(parts, part, '/');) {
            if (part.empty())
                continue;
            nid = Child(Node(nid), part);
            if (nid < 0 || Node(nid).type != Directory)
                return -1;
        }
        return nid;
    }

    int64_t DirectoryAt(const std::string& path) const
    {
        int64_t system = Child(Node(root_), "system"), result = system < 0 ? -1 : Under(system, path);
        return result < 0 ? Under(root_, path) : result;
    }

    static int Index(View pack, int bits, int position)
    {
        int bit = bits * position;
        return (U32(pack, bit / 8) >> (bit % 8)) & 0x3fff;
    }

    static int BlocksBefore(View pack, int bits, int position, bool big)
    {
        int blocks = big ? 0 : 1, i = position;
        while (i > 0) {
            int previous = Index(pack, bits, --i), low = previous & 4095;
            if ((previous >> 12) != Nonhead)
                blocks++;
            else if (!big) {
                Require(low > 0, "Invalid EROFS backreference");
                i -= low;
                if (i >= 0)
                    blocks++;
            }
            else if (low & BlockCountFlag) {
                i--;
                blocks += low & ~BlockCountFlag;
            }
            else {
                Require(low >= 2, "Invalid EROFS backreference");
                i -= low - 2;
            }
        }
        return blocks;
    }

    static size_t Length(View bytes, size_t& cursor, size_t length)
    {
        if (length != 15)
            return length;
        int next;
        do {
            Require(cursor < bytes.size(), "Truncated EROFS LZ4 length");
            next = bytes[cursor++];
            length += next;
            Require(length <= 12 * 1024 * 1024, "EROFS LZ4 length exceeds cluster limit");
        } while (next == 255);
        return length;
    }

    static Bytes Decompress(View bytes, size_t size)
    {
        Bytes output(size);
        size_t cursor = 0;
        for (size_t position = 0; position < size;) {
            Require(cursor < bytes.size(), "Truncated EROFS LZ4 cluster");
            int token = bytes[cursor++];
            size_t literals = Length(bytes, cursor, token >> 4);
            Require(literals <= size - position && literals <= bytes.size() - cursor, "Invalid EROFS LZ4 literals");
            std::copy_n(bytes.data() + cursor, literals, output.data() + position);
            position += literals;
            cursor += literals;
            if (position == size)
                break;
            Require(cursor <= bytes.size() && bytes.size() - cursor >= 2, "Truncated EROFS LZ4 match");
            size_t distance = U16(bytes, cursor);
            cursor += 2;
            size_t length = Length(bytes, cursor, token & 15) + 4;
            Require(distance > 0 && distance <= position && length <= size - position, "Invalid EROFS LZ4 match");
            // Copy only initialized history on each iteration. This preserves
            // overlapping LZ4 matches without per-byte debug iterator checks.
            auto* target = output.data();
            for (size_t remaining = length, available = distance; remaining;) {
                size_t count = std::min(available, remaining);
                std::memcpy(target + position, target + position - available, count);
                position += count;
                remaining -= count;
                available += count;
            }
        }
        return output;
    }

    void CopyCluster(const Cluster& cluster, int blocks, int64_t end, std::ostream& output) const
    {
        int64_t length = end - cluster.logical;
        Require(length > 0 && length <= 12 * 1024 * 1024 && blocks > 0 && blocks <= 1024 * 1024 / block_,
                "Invalid EROFS cluster bounds");
        auto bytes = image_.Read(cluster.physical * block_, blocks * block_);
        if (!cluster.type) {
            Require(static_cast<uint64_t>(length) <= bytes.size(), "Truncated EROFS uncompressed cluster");
            output.write(reinterpret_cast<const char*>(bytes.data()), length);
        }
        else {
            size_t padding = 0;
            if (zeroPadded_) {
                while (padding < block_ && !bytes[padding])
                    padding++;
                Require(padding < block_, "Missing EROFS LZ4 data");
            }
            auto decoded = Decompress(View(bytes).subspan(padding), static_cast<size_t>(length));
            output.write(reinterpret_cast<const char*>(decoded.data()), decoded.size());
        }
    }

    void Compressed(const Inode& node, std::ostream& output) const
    {
        int64_t at = (node.inlined + 7) & ~7ll;
        auto header = image_.Read(at, 8);
        int flags = U16(header, 4);
        bool big = (flags & 2) != 0;
        Require(block_ == 4096 && (flags & ~7) == 0 && ((flags & 4) != 0) == big && !header[6] && !header[7],
                "Unsupported EROFS LZ4 cluster layout");
        int64_t start = at + 8;
        int count = static_cast<int>((node.size + block_ - 1) / block_),
            initial = static_cast<int>((32 - start % 32) % 32) / 4;
        int compact = flags & 1 ? std::max(0, count - initial) / 16 * 16 : 0;
        std::optional<Cluster> head;
        int physicalBlocks = 1;
        for (int first = 0; first < count;) {
            bool packed = first >= initial && first < initial + compact;
            int entries = packed ? 16 : 2, bits = packed ? 14 : 16;
            auto pack = image_.Read(start + 4ll * first - 2ll * std::max(0, std::min(compact, first - initial)),
                                    packed ? 32 : 8);
            for (int i = 0; i < entries && first + i < count; i++) {
                int value = Index(pack, bits, i), type = value >> 12, low = value & 4095;
                if (type == Nonhead) {
                    Require(head.has_value(), "EROFS file starts inside a compressed cluster");
                    if (big && (low & BlockCountFlag)) {
                        Require(head->index + 1 == first + i, "Misplaced EROFS physical cluster size");
                        physicalBlocks = low & ~BlockCountFlag;
                    }
                    continue;
                }
                int64_t logical = int64_t(first + i) * block_ + low;
                if (logical == node.size && first + i == count - 1 && type == 0 && head) {
                    CopyCluster(*head, physicalBlocks, logical, output);
                    return;
                }
                Require(type <= 1 && logical < node.size, "Invalid EROFS cluster type/offset");
                if (head)
                    CopyCluster(*head, physicalBlocks, logical, output);
                else
                    Require(logical == 0, "Missing first EROFS cluster");
                head = Cluster{first + i, type, logical,
                               int64_t(U32(pack, pack.size() - 4)) + BlocksBefore(pack, bits, i, big)};
                physicalBlocks = 1;
            }
            first += entries;
        }
        Require(head.has_value(), "Missing EROFS compressed contents");
        CopyCluster(*head, physicalBlocks, node.size, output);
    }

    void CopyData(const Inode& node, std::ostream& output) const
    {
        if (!node.size)
            return;
        if (node.layout == 3) {
            Compressed(node, output);
            return;
        }
        Bytes bytes(static_cast<size_t>(std::min<int64_t>(node.size, 256 * 1024)));
        for (int64_t offset = 0; offset < node.size;) {
            size_t count = static_cast<size_t>(std::min<int64_t>(bytes.size(), node.size - offset));
            Flat(node, offset, std::span(bytes).first(count));
            output.write(reinterpret_cast<const char*>(bytes.data()), count);
            offset += count;
        }
    }

public:
    explicit Erofs(Image image) : image_(std::move(image))
    {
        auto sb = image_.Read(1024, 128);
        int bits = sb[12];
        uint32_t incompat = U32(sb, 80);
        Require(bits >= 9 && bits <= 12 && !sb[90] && (incompat & ~3u) == 0, "Unsupported EROFS filesystem features");
        block_ = 1 << bits;
        metadata_ = int64_t(U32(sb, 40)) * block_;
        root_ = U16(sb, 14);
        zeroPadded_ = (incompat & 1) != 0;
    }

    std::vector<Entry> List(const std::string& directory) const override
    {
        std::vector<Entry> entries;
        auto nid = DirectoryAt(directory);
        if (nid >= 0)
            Visit(Node(nid), [&](auto name, auto number) {
                entries.push_back({name, Node(number).type});
                return false;
            });
        return entries;
    }

    bool Copy(const std::string& directory, const std::string& name, std::ostream& output) const override
    {
        auto parent = DirectoryAt(directory), nid = parent < 0 ? -1 : Child(Node(parent), name);
        if (nid < 0)
            return false;
        auto node = Node(nid);
        Require(node.type == Regular && node.size >= 0 && node.size <= 512ll * 1024 * 1024,
                "Invalid EROFS file: " + name);
        CopyData(node, output);
        return true;
    }

    std::string Link(const std::string& directory, const std::string& name) const override
    {
        auto parent = DirectoryAt(directory), nid = parent < 0 ? -1 : Child(Node(parent), name);
        if (nid < 0)
            return {};
        auto node = Node(nid);
        if (node.type != Symlink)
            return {};
        Require(node.size > 0 && node.size < 4096, "Invalid EROFS symlink size");
        std::ostringstream bytes(std::ios::binary);
        CopyData(node, bytes);
        auto result = bytes.str();
        Path(result);
        return result;
    }
};
}

std::shared_ptr<Partition> OpenErofs(Image image)
{
    return std::make_shared<Erofs>(std::move(image));
}
}

#include "Image.h"
#include <sstream>

namespace stone::image {
namespace {
class Ext4 final : public Partition {
    Image image_;
    int block_, inodeSize_, descriptorSize_;
    int64_t blocks_, inodes_, perGroup_, descriptorTable_;

    struct Extent {
        int64_t logical;
        int count;
        int64_t physical;
    };

    using Visitor = std::function<bool(const std::string&, int64_t)>;

    Bytes Inode(int64_t number) const
    {
        Require(number > 0 && number <= inodes_, "Invalid ext4 inode number");
        int64_t group = (number - 1) / perGroup_, index = (number - 1) % perGroup_;
        auto descriptor = image_.Read(descriptorTable_ + group * descriptorSize_, descriptorSize_);
        uint64_t encoded = U32(descriptor, 8) | (descriptorSize_ >= 64 ? uint64_t(U32(descriptor, 40)) << 32 : 0);
        Require(encoded <= INT64_MAX, "Invalid ext4 inode table offset");
        int64_t table = static_cast<int64_t>(encoded);
        Require(table > 0 && table < blocks_ && index * inodeSize_ <= (blocks_ - table) * block_ - inodeSize_,
                "Invalid ext4 inode table");
        return image_.Read(table * block_ + index * inodeSize_, inodeSize_);
    }

    static int64_t Size(View inode)
    {
        return static_cast<int64_t>(uint64_t(U32(inode, 4)) | (uint64_t(U32(inode, 108)) << 32));
    }

    void Extents(View node, int depth, int64_t first, int64_t limit, std::vector<Extent>& result) const
    {
        int entries = U16(node, 2), capacity = U16(node, 4);
        Require(U16(node, 0) == 0xf30a && U16(node, 6) == depth && entries <= capacity &&
                    capacity <= (node.size() - 12) / 12,
                "Invalid ext4 extent header");
        int64_t next = first;
        for (int index = 0; index < entries; index++) {
            int at = 12 + index * 12;
            int64_t logical = U32(node, at);
            Require(logical >= next && logical < limit, "Invalid ext4 logical block");
            if (depth > 0) {
                int64_t physical = U32(node, at + 4) | (int64_t(U16(node, at + 8)) << 32);
                int64_t end = index + 1 < entries ? U32(node, at + 12) : limit;
                Require(physical > 0 && physical < blocks_ && end > logical && end <= limit,
                        "Invalid ext4 extent index");
                Extents(image_.Read(physical * block_, block_), depth - 1, logical, end, result);
                next = end;
            }
            else {
                int encoded = U16(node, at + 4), length = encoded > 32768 ? encoded - 32768 : encoded;
                int64_t physical = U32(node, at + 8) | (int64_t(U16(node, at + 6)) << 32);
                Require(length > 0 && length <= limit - logical && physical > 0 && physical < blocks_ &&
                            length <= blocks_ - physical && result.size() < 1'000'000,
                        "Invalid ext4 extent bounds");
                result.push_back({logical, length, encoded > 32768 ? 0 : physical});
                next = logical + length;
            }
        }
    }

    std::vector<Extent> Extents(View inode, int64_t size) const
    {
        Require((U32(inode, 32) & 0x80000) != 0, "Ext4 inode does not use extents");
        auto root = inode.subspan(40, 60);
        int depth = U16(root, 6);
        Require(depth <= 5, "Invalid ext4 extent tree depth");
        std::vector<Extent> result;
        Extents(root, depth, 0, (size + block_ - 1) / block_, result);
        return result;
    }

    int64_t Visit(int64_t parent, const Visitor& visitor) const
    {
        auto inode = Inode(parent);
        int64_t size = Size(inode);
        Require((U16(inode, 0) & 0xf000) == Directory && size > 0 && size % block_ == 0 && size <= 64ll * 1024 * 1024,
                "Invalid ext4 directory inode/size");
        for (const auto& extent : Extents(inode, size)) {
            if (!extent.physical)
                continue;
            for (int block = 0; block < extent.count; block++) {
                auto bytes = image_.Read((extent.physical + block) * block_, block_);
                for (int at = 0; at < block_;) {
                    Require(block_ - at >= 8, "Truncated ext4 directory entry");
                    int length = U16(bytes, at + 4);
                    if (block_ == 65536)
                        length = length == 65535 || length == 0 ? 65536 : (length & 0xfffc) | ((length & 3) << 16);
                    int nameSize = bytes[at + 6];
                    Require(length >= 8 && length % 4 == 0 && length <= block_ - at && nameSize <= length - 8,
                            "Invalid ext4 directory entry");
                    int64_t number = U32(bytes, at);
                    if (number) {
                        auto name = Text(View(bytes).subspan(at + 8, nameSize));
                        if (name != "." && name != ".." && visitor(name, number))
                            return number;
                    }
                    at += length;
                }
            }
        }
        return 0;
    }

    int64_t Child(int64_t parent, const std::string& name) const
    {
        return Visit(parent, [&](const auto& entry, auto) {
            return name == entry;
        });
    }

    int64_t Under(int64_t number, const std::string& path) const
    {
        std::istringstream parts(path);
        for (std::string part; std::getline(parts, part, '/');) {
            if (part.empty())
                continue;
            number = Child(number, part);
            if (!number || (U16(Inode(number), 0) & 0xf000) != Directory)
                return 0;
        }
        return number;
    }

    int64_t DirectoryAt(const std::string& path) const
    {
        int64_t system = Child(2, "system"), number = system ? Under(system, path) : 0;
        return number ? number : Under(2, path);
    }

    void CopyData(View inode, int64_t size, std::ostream& output) const
    {
        if (!size)
            return;
        Bytes bytes(static_cast<size_t>(std::min<int64_t>(size, 256 * 1024)));
        auto zero = [&](int64_t length) {
            Require(length >= 0, "Overlapping ext4 extents");
            std::fill(bytes.begin(), bytes.end(), uint8_t{0});
            while (length) {
                auto count = std::min<int64_t>(bytes.size(), length);
                output.write(reinterpret_cast<const char*>(bytes.data()), count);
                length -= count;
            }
        };
        int64_t position = 0;
        for (const auto& extent : Extents(inode, size)) {
            int64_t start = extent.logical * block_,
                    length = std::min<int64_t>(size - start, int64_t(extent.count) * block_);
            zero(start - position);
            if (!extent.physical)
                zero(length);
            else
                for (int64_t copied = 0; copied < length;) {
                    size_t count = static_cast<size_t>(std::min<int64_t>(bytes.size(), length - copied));
                    image_.Read(extent.physical * block_ + copied, std::span(bytes).first(count));
                    output.write(reinterpret_cast<const char*>(bytes.data()), count);
                    copied += count;
                }
            position = start + length;
        }
        zero(size - position);
    }

public:
    explicit Ext4(Image image) : image_(std::move(image))
    {
        auto sb = image_.Read(1024, 1024);
        Require(U16(sb, 56) == 0xef53, "Unsupported Android filesystem (expected ext4 or EROFS)");
        auto log = U32(sb, 24);
        Require(log <= 6, "Invalid ext4 block size");
        block_ = 1024 << log;
        inodeSize_ = U16(sb, 88);
        auto features = U32(sb, 96);
        Require((features & (4 | 8 | 16)) == 0, "Unsupported ext4 journal or block group layout");
        bool wide = (features & 0x80) != 0;
        descriptorSize_ = wide ? U16(sb, 254) : 32;
        inodes_ = U32(sb, 0);
        perGroup_ = U32(sb, 40);
        int64_t first = U32(sb, 20);
        Require(first == (block_ == 1024 ? 1 : 0) && inodes_ >= 2 && perGroup_ > 0 && inodeSize_ >= 128 &&
                    inodeSize_ <= block_ && block_ % inodeSize_ == 0 && descriptorSize_ >= (wide ? 64 : 32) &&
                    descriptorSize_ <= block_ && block_ % descriptorSize_ == 0,
                "Invalid ext4 geometry");
        uint64_t blocks = U32(sb, 4) | (wide ? uint64_t(U32(sb, 336)) << 32 : 0);
        Require(blocks <= INT64_MAX, "Invalid ext4 block count");
        blocks_ = static_cast<int64_t>(blocks);
        Require(blocks_ > first + 1 && blocks_ <= image_.size / block_, "Invalid ext4 block count");
        descriptorTable_ = (first + 1) * block_;
    }

    std::vector<Entry> List(const std::string& directory) const override
    {
        std::vector<Entry> result;
        auto number = DirectoryAt(directory);
        if (number)
            Visit(number, [&](auto name, auto child) {
                result.push_back({name, U16(Inode(child), 0) & 0xf000});
                return false;
            });
        return result;
    }

    bool Copy(const std::string& directory, const std::string& name, std::ostream& output) const override
    {
        auto parent = DirectoryAt(directory), number = parent ? Child(parent, name) : 0;
        if (!number)
            return false;
        auto inode = Inode(number);
        int64_t size = Size(inode);
        Require((U16(inode, 0) & 0xf000) == Regular && size >= 0 && size <= 512ll * 1024 * 1024,
                "Invalid ext4 file inode/size: " + name);
        CopyData(inode, size, output);
        return true;
    }

    std::string Link(const std::string& directory, const std::string& name) const override
    {
        auto parent = DirectoryAt(directory), number = parent ? Child(parent, name) : 0;
        if (!number)
            return {};
        auto inode = Inode(number);
        if ((U16(inode, 0) & 0xf000) != Symlink)
            return {};
        int64_t size = Size(inode);
        Require(size > 0 && size < 4096, "Invalid ext4 symlink size");
        if (!(U32(inode, 32) & 0x80000) && size <= 60)
            return Text(View(inode).subspan(40, static_cast<size_t>(size)));
        std::ostringstream output(std::ios::binary);
        CopyData(inode, size, output);
        auto text = output.str();
        Path(text);
        return text;
    }
};
}

std::shared_ptr<Partition> OpenExt4(Image image)
{
    return std::make_shared<Ext4>(std::move(image));
}
}

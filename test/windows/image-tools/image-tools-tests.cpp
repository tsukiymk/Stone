#include "../../../src/windows/image-tools/Image.h"
#include <miniz.h>
#include <fstream>
#include <iostream>
#include <sstream>

using namespace stone::image;

namespace {
void Expect(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error("TEST: " + message);
}

void Reject(const std::function<void()>& action)
{
    try {
        action();
    }
    catch (const std::runtime_error&) {
        return;
    }
    throw std::runtime_error("Accepted malformed image metadata");
}

template <class T> void Put(Bytes& bytes, size_t at, T value)
{
    Expect(at <= bytes.size() && sizeof(value) <= bytes.size() - at, "Fixture bounds");
    std::memcpy(bytes.data() + at, &value, sizeof(value));
}

void U16At(Bytes& bytes, size_t at, int value)
{
    Put(bytes, at, static_cast<uint16_t>(value));
}

void U32At(Bytes& bytes, size_t at, uint64_t value)
{
    Put(bytes, at, static_cast<uint32_t>(value));
}

Bytes Data(std::string_view text)
{
    return {text.begin(), text.end()};
}

void Append(Bytes& target, View bytes)
{
    target.insert(target.end(), bytes.begin(), bytes.end());
}

Bytes Field(int field, View bytes)
{
    Bytes result{static_cast<uint8_t>(field * 8 + 2)};
    for (size_t size = bytes.size();; size >>= 7) {
        result.push_back(static_cast<uint8_t>((size & 127) | (size > 127 ? 128 : 0)));
        if (size <= 127)
            break;
    }
    Append(result, bytes);
    return result;
}

Bytes TextField(int field, const std::string& text)
{
    return Field(field, Data(text));
}

Bytes Jar(const std::string& path, int kind = 1, const std::string& minimum = "", const std::string& maximum = "")
{
    auto bytes = TextField(1, path);
    bytes.push_back(16);
    bytes.push_back(static_cast<uint8_t>(kind));
    Append(bytes, TextField(3, minimum));
    Append(bytes, TextField(4, maximum));
    return Field(1, bytes);
}

void Classpaths()
{
    Properties properties{{"ro.build.version.sdk", "37"}};
    std::string first = "/system/framework/framework.jar", second = "/apex/com.android.art/javalib/core-oj.jar";
    auto check = [&](const Bytes& bytes, const std::vector<std::string>& expected) {
        Expect(BootClassPath(bytes, "", properties) == expected, "Classpath order/SDK filtering");
    };
    auto bytes = Jar(first);
    for (const auto& part : {Jar(second, 1, "37", "37"), Jar(first, 3), Jar(first, 1, "38"), Jar(first, 1, "", "36"),
                             Jar(first, 1, "Future"), TextField(9, "unknown")})
        Append(bytes, part);
    check(bytes, {first, second});
    check(Jar(first, 1, "", "Future"), {first});
    properties["ro.build.version.codename"] = "Future";
    properties["ro.build.version.known_codenames"] = "Previous,Future";
    bytes = Jar(first, 1, "Previous", "Future");
    Append(bytes, Jar(second, 1, "Future"));
    Append(bytes, Jar(first, 1, "", "37"));
    Append(bytes, Jar(first, 1, "", "Previous"));
    check(bytes, {first, second});
    for (const auto& path : {"/system/../data/code.jar", "/system/./code.jar", "/data/code.jar", "/system//code.jar",
                             "/system/code.jar:other.jar"})
        Reject([&] {
            BootClassPath(Jar(path), "", properties);
        });
    Reject([&] {
        BootClassPath(Jar(first), "/apex/com.android.art/", properties);
    });
    for (const Bytes& bad : {Bytes{10, 127, 0}, Bytes{10, 128}, Bytes{0}, Field(1, Bytes{10, 1, 255}),
                             Field(1, Bytes{16, 255, 255, 255, 255, 255, 255, 255, 255, 255, 2})})
        Reject([&] {
            BootClassPath(bad, "", properties);
        });
    Expect(ApexName(TextField(1, "com.android.art")) == "com.android.art", "Canonical APEX name");
    Reject([&] {
        ApexName(TextField(1, "../escape"));
    });
}

struct Node {
    std::string name, link;
    Bytes bytes;
    bool directory = true;
    int inode = 0;
    std::vector<std::unique_ptr<Node>> children;
};

std::unique_ptr<Node> Tree(const std::map<std::string, Bytes>& files)
{
    auto root = std::make_unique<Node>();
    for (const auto& [path, bytes] : files) {
        Node* parent = root.get();
        std::istringstream parts(path);
        std::vector<std::string> names;
        for (std::string part; std::getline(parts, part, '/');)
            names.push_back(part);
        for (size_t i = 0; i + 1 < names.size(); i++) {
            Node* next = nullptr;
            for (auto& child : parent->children)
                if (child->name == names[i])
                    next = child.get();
            if (!next) {
                auto node = std::make_unique<Node>();
                node->name = names[i];
                next = node.get();
                parent->children.push_back(std::move(node));
            }
            parent = next;
        }
        auto node = std::make_unique<Node>();
        node->name = names.back();
        node->directory = false;
        node->bytes = bytes;
        parent->children.push_back(std::move(node));
    }
    return root;
}

void Alias(Node& parent, std::string name, std::string target)
{
    auto node = std::make_unique<Node>();
    node->name = std::move(name);
    node->link = std::move(target);
    node->directory = false;
    parent.children.push_back(std::move(node));
}

Bytes Ext4(Node& root)
{
    std::vector<Node*> nodes;
    std::function<void(Node&)> collect = [&](Node& node) {
        node.inode = static_cast<int>(nodes.size()) + 2;
        nodes.push_back(&node);
        for (auto& child : node.children)
            collect(*child);
    };
    collect(root);
    constexpr int block = 4096;
    int next = 2 + (static_cast<int>(nodes.size()) + 2 + 15) / 16;
    Bytes metadata(next * block);
    std::vector<std::pair<int, Bytes>> contents;
    U32At(metadata, 1024, nodes.size() + 2);
    U32At(metadata, 1048, 2);
    U32At(metadata, 1064, nodes.size() + 2);
    U16At(metadata, 1080, 0xef53);
    U16At(metadata, 1112, 256);
    U32At(metadata, 1120, 0x40);
    U32At(metadata, block + 8, 2);
    for (auto* node : nodes) {
        int inode = 2 * block + (node->inode - 1) * 256;
        U16At(metadata, inode,
              node->directory      ? Partition::Directory
              : node->link.empty() ? Partition::Regular
                                   : Partition::Symlink);
        Bytes bytes = node->link.empty() ? node->bytes : Data(node->link);
        if (node->directory) {
            bytes.resize(block);
            int offset = 0;
            for (size_t i = 0; i < node->children.size(); i++) {
                auto& child = *node->children[i];
                auto name = Data(child.name);
                int length =
                    i + 1 == node->children.size() ? block - offset : (8 + static_cast<int>(name.size()) + 3) & ~3;
                U32At(bytes, offset, child.inode);
                U16At(bytes, offset + 4, length);
                bytes[offset + 6] = static_cast<uint8_t>(name.size());
                std::copy(name.begin(), name.end(), bytes.begin() + offset + 8);
                offset += length;
            }
            if (node->children.empty())
                U16At(bytes, 4, block);
        }
        U32At(metadata, inode + 4, bytes.size());
        if (!node->link.empty() && bytes.size() <= 60) {
            std::copy(bytes.begin(), bytes.end(), metadata.begin() + inode + 40);
            continue;
        }
        U32At(metadata, inode + 32, 0x80000);
        U16At(metadata, inode + 40, 0xf30a);
        U16At(metadata, inode + 42, bytes.empty() ? 0 : 1);
        U16At(metadata, inode + 44, 4);
        if (bytes.empty())
            continue;
        int blocks = (static_cast<int>(bytes.size()) + block - 1) / block;
        U16At(metadata, inode + 56, blocks);
        U32At(metadata, inode + 60, next);
        contents.emplace_back(next, bytes);
        next += blocks;
    }
    U32At(metadata, 1028, next);
    Bytes image(next * block);
    std::copy(metadata.begin(), metadata.end(), image.begin());
    for (const auto& [at, bytes] : contents)
        std::copy(bytes.begin(), bytes.end(), image.begin() + at * block);
    return image;
}

Image Memory(Bytes bytes)
{
    auto data = std::make_shared<Bytes>(std::move(bytes));
    return {[data](int64_t offset, std::span<uint8_t> out) {
                std::copy_n(data->data() + offset, out.size(), out.data());
            },
            static_cast<int64_t>(data->size())};
}

Bytes Sparse(View bytes)
{
    Bytes header(40);
    U32At(header, 0, 0xed26ff3a);
    U16At(header, 4, 1);
    U16At(header, 8, 28);
    U16At(header, 10, 12);
    U32At(header, 12, 4096);
    U32At(header, 16, bytes.size() / 4096);
    U32At(header, 20, 1);
    U16At(header, 28, 0xcac1);
    U32At(header, 32, bytes.size() / 4096);
    U32At(header, 36, bytes.size() + 12);
    Append(header, bytes);
    return header;
}

Bytes Gpt(View bytes)
{
    Bytes image(4096);
    auto magic = Data("EFI PART");
    std::copy(magic.begin(), magic.end(), image.begin() + 512);
    Put<int64_t>(image, 584, 2);
    U32At(image, 592, 1);
    U32At(image, 596, 128);
    image[1024] = 1;
    Put<int64_t>(image, 1056, 8);
    Put<int64_t>(image, 1064, 7 + bytes.size() / 512);
    std::wstring name = L"system_a";
    std::memcpy(image.data() + 1080, name.data(), name.size() * 2);
    Append(image, bytes);
    return image;
}

Bytes Super(View bytes)
{
    Bytes image(32768);
    U32At(image, 4096, 0x616c4467);
    U32At(image, 4136, 4096);
    U32At(image, 4140, 2);
    int header = 12288, table = header + 128;
    U32At(image, header, 0x414c5030);
    U16At(image, header + 4, 10);
    U32At(image, header + 8, 128);
    U32At(image, header + 44, 76);
    U32At(image, header + 84, 1);
    U32At(image, header + 88, 52);
    U32At(image, header + 92, 52);
    U32At(image, header + 96, 1);
    U32At(image, header + 100, 24);
    auto name = Data("system_a");
    std::copy(name.begin(), name.end(), image.begin() + table);
    U32At(image, table + 44, 1);
    Put<int64_t>(image, table + 52, bytes.size() / 512);
    Put<int64_t>(image, table + 64, 64);
    Append(image, bytes);
    return image;
}

Bytes Erofs(View contents, bool inlined)
{
    Bytes image(16384);
    U32At(image, 1024, 0xe0f5e1e2);
    image[1036] = 12;
    U32At(image, 1064, 1);
    U16At(image, 4100, Partition::Directory);
    U32At(image, 4104, 16);
    U32At(image, 4112, 2);
    Put<int64_t>(image, 8192, 2);
    U16At(image, 8200, 12);
    auto name = Data("file");
    std::copy(name.begin(), name.end(), image.begin() + 8204);
    U16At(image, 4160, inlined ? 4 : 0);
    U16At(image, 4164, Partition::Regular);
    U32At(image, 4168, contents.size());
    U32At(image, 4176, 3);
    std::copy(contents.begin(), contents.end(), image.begin() + (inlined ? 4192 : 12288));
    return image;
}

Bytes Elf()
{
    Bytes bytes(64);
    bytes[0] = 0x7f;
    bytes[1] = 'E';
    bytes[2] = 'L';
    bytes[3] = 'F';
    bytes[4] = 2;
    bytes[5] = 1;
    U16At(bytes, 54, 56);
    return bytes;
}

void Filesystems()
{
    auto text = Data("ro.build.version.sdk=37\n");
    auto tree = Tree({{"build.prop", text}});
    Alias(*tree, "alias", "build.prop");
    auto ext4 = Ext4(*tree);
    for (const auto& bytes : {ext4, Sparse(ext4), Gpt(ext4), Super(ext4)}) {
        auto partition = OpenPartition(OpenSystem(Memory(bytes)));
        Expect(partition->Read("", "build.prop") == text && partition->Link("", "alias") == "build.prop",
               "ext4 / sparse / GPT / super extraction");
    }
    for (bool inlined : {false, true})
        Expect(OpenPartition(Memory(Erofs(text, inlined)))->Read("", "file") == text, "EROFS flat/inline extraction");
    auto backup = Super(ext4);
    std::copy_n(backup.data() + 12288, 4096, backup.data() + 20480);
    backup[12288] = 0;
    Expect(OpenPartition(OpenSystem(Memory(backup)))->Read("", "build.prop") == text, "Backup super metadata recovery");
    auto bad = Sparse(ext4);
    U32At(bad, 36, 12);
    Reject([&] {
        OpenSystem(Memory(bad));
    });
    auto elf = Elf();
    Expect(ElfDependencies(elf).empty(), "ELF without DT_NEEDED");
    elf[4] = 1;
    Reject([&] {
        ElfDependencies(elf);
    });
}

Bytes Zip(const std::string& name, View bytes, bool deflate = true)
{
    Bytes encoded;
    if (deflate) {
        for (size_t offset = 0; offset < bytes.size();) {
            size_t count = std::min<size_t>(65535, bytes.size() - offset);
            encoded.push_back(offset + count == bytes.size() ? 1 : 0);
            size_t at = encoded.size();
            encoded.resize(at + 4);
            U16At(encoded, at, static_cast<int>(count));
            U16At(encoded, at + 2, static_cast<int>(~count));
            Append(encoded, bytes.subspan(offset, count));
            offset += count;
        }
    }
    else
        Append(encoded, bytes);
    auto crc = mz_crc32(0, bytes.data(), bytes.size());
    Bytes result(30);
    U32At(result, 0, 0x04034b50);
    U16At(result, 4, 20);
    U16At(result, 8, deflate ? 8 : 0);
    U32At(result, 14, crc);
    U32At(result, 18, encoded.size());
    U32At(result, 22, bytes.size());
    U16At(result, 26, static_cast<int>(name.size()));
    Append(result, Data(name));
    Append(result, encoded);
    size_t central = result.size();
    Bytes record(46);
    U32At(record, 0, 0x02014b50);
    U16At(record, 4, 20);
    U16At(record, 6, 20);
    U16At(record, 10, deflate ? 8 : 0);
    U32At(record, 16, crc);
    U32At(record, 20, encoded.size());
    U32At(record, 24, bytes.size());
    U16At(record, 28, static_cast<int>(name.size()));
    Append(result, record);
    Append(result, Data(name));
    Bytes end(22);
    U32At(end, 0, 0x06054b50);
    U16At(end, 8, 1);
    U16At(end, 10, 1);
    U32At(end, 12, result.size() - central);
    U32At(end, 16, central);
    Append(result, end);
    return result;
}

void Write(const fs::path& path, View bytes)
{
    Atomic(path, [&](auto& output) {
        output.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    });
}

Bytes Read(const fs::path& path)
{
    auto native = path;
    std::ifstream input(native.make_preferred(), std::ios::binary);
    Expect(bool(input), "Missing output file");
    return {std::istreambuf_iterator<char>(input), {}};
}

int Native(const fs::path& executable, const std::vector<fs::path>& arguments)
{
    auto quote = [](const fs::path& path) {
        auto value = path.wstring();
        Expect(value.empty() || value.back() != L'\\', "Fixture argument must not end in slash");
        return L"\"" + value + L"\"";
    };
    std::wstring command = quote(executable);
    for (const auto& value : arguments)
        command += L' ' + quote(value);
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    Expect(CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                          nullptr, &startup, &process),
           "Start native image tool");
    CloseHandle(process.hThread);
    auto status = WaitForSingleObject(process.hProcess, 30000);
    if (status != WAIT_OBJECT_0)
        TerminateProcess(process.hProcess, ERROR_TIMEOUT);
    DWORD code = 1;
    bool exited = GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hProcess);
    Expect(status == WAIT_OBJECT_0 && exited, "Native image tool timeout");
    return static_cast<int>(code);
}

void EndToEnd(const fs::path& root, const fs::path& executable)
{
    std::map<std::string, Bytes> apexFiles{
        {"apex_manifest.pb", TextField(1, "com.android.art")},
        {"etc/classpaths/bootclasspath.pb", Jar("/apex/com.android.art/javalib/core-oj.jar")},
        {"javalib/core-oj.jar", {0x50, 0x4b}},
        {"bin/dex2oat64", Elf()}};
    for (const auto* name :
         {"libart.so", "libartpalette-system.so", "libjavacore.so", "libopenjdkjvm.so", "libopenjdk.so",
          "libjavacrypto.so", "libjnigraphics.so", "libandroid.so", "libandroid_runtime.so", "libhwui.so"})
        apexFiles[std::string("lib64/") + name] = Elf();
    auto apex = Zip("apex_payload.img", Ext4(*Tree(apexFiles)));
    std::map<std::string, Bytes> files{{"build.prop", Data("ro.build.version.sdk=37\n")},
                                       {"etc/classpaths/bootclasspath.pb", Jar("/system/framework/framework.jar")},
                                       {"framework/framework.jar", {0x50, 0x4b}},
                                       {"framework/framework-res.apk", {0x50, 0x4b}},
                                       {"usr/keychars/Virtual.kcm", {42}},
                                       {"fonts/font.ttf", {1, 2, 3}},
                                       {"apex/com.google.android.art.capex", Zip("original_apex", apex, false)}};
    auto tree = Tree(files);
    for (auto& child : tree->children)
        if (child->name == "fonts")
            Alias(*child, "alias.ttf", "/system/fonts/font.ttf");
    auto image = root / L"system.img", cache = root / L"cache", index = root / L"index.txt";
    Write(image, Sparse(Ext4(*tree)));
    SetEnvironmentVariableW(L"JAVA_HOME", L"C:\\stone-no-jdk");
    SetEnvironmentVariableW(L"DOTNET_ROOT", L"C:\\stone-no-dotnet");
    wchar_t system[32768]{};
    Expect(GetSystemDirectoryW(system, 32768) > 0, "System directory");
    SetEnvironmentVariableW(L"PATH", system);
    Expect(Native(executable, {L"--output-file", index, image, cache, L"libandroid_runtime.so", L"libhwui.so"}) == 0,
           "Cold C++ preparation without JDK/.NET");
    auto output = Path(Text(Read(index)));
    Expect(Read(output / L"system/etc/stone-bootclasspath.list") ==
               Data("/apex/com.android.art/javalib/core-oj.jar\n/system/framework/framework.jar\n"),
           "Boot classpath publication/order");
    Expect(Read(output / L"lib64/libart.so") == Elf(), "ART library bytes preserved");
    Expect(Read(output / L"system/fonts/alias.ttf") == Bytes({1, 2, 3}), "Font alias resolution");
    Expect(Native(executable, {L"--output-file", index, image, cache}) == 0, "Repeated native preparation");
    auto extracted = root / L"extracted.prop";
    Expect(Native(executable, {L"--extract-file", image, L"", L"build.prop", extracted}) == 0 &&
               Read(extracted) == files["build.prop"],
           "Native diagnostic extraction");
    fs::remove(index);
    Write(image, Bytes{0, 0, 0, 0});
    Expect(Native(executable, {L"--output-file", index, image, cache}) == 1 && !fs::exists(index),
           "Invalid image must not publish success");
    auto archive = root / L"bad.apex";
    Write(archive, Zip("../apex_payload.img", Bytes{1}));
    Reject([&] {
        ZipEntry(archive, "apex_payload.img", root / L"payload.img");
    });
    auto corrupt = Zip("apex_payload.img", Bytes{1, 2, 3}, false);
    corrupt[30 + std::string("apex_payload.img").size()] ^= 1;
    Write(archive, corrupt);
    Reject([&] {
        ZipEntry(archive, "apex_payload.img", root / L"payload.img");
    });
    Expect(!GetModuleHandleW(L"jvm.dll"), "Image tests loaded a Windows JVM");
}
}

int wmain(int argc, wchar_t** argv)
{
    try {
        Expect(argc == 3, "Expected output and native executable paths");
        auto root = fs::path(argv[1]) / L"路径 with spaces";
        DirectorySafe(root);
        Classpaths();
        Filesystems();
        EndToEnd(root, argv[2]);
        std::cout
            << "PASS C++ image preparation without JDK/.NET, ext4/EROFS/sparse/GPT/super, APEX ZIP CRC, SDK filtering, aliases and failure publication\n";
        return 0;
    }
    catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

#include "Image.h"
#include "../host/WindowsPath.h"
#include <miniz.h>
#include <objbase.h>
#include <deque>
#include <exception>
#include <fstream>
#include <optional>
#include <regex>
#include <sstream>

namespace stone::image {
void DirectorySafe(const fs::path& value)
{
    auto native = fs::absolute(value).make_preferred().wstring();
    if (native.starts_with(L"\\\\?\\UNC\\"))
        native = L"\\\\" + native.substr(8);
    else if (native.starts_with(L"\\\\?\\"))
        native.erase(0, 4);
    fs::path path(native);
    size_t rootLength = path.root_path().native().size();
    if (native.starts_with(L"\\\\")) {
        auto server = native.find(L'\\', 2);
        auto share = server == std::wstring::npos ? server : native.find(L'\\', server + 1);
        rootLength = share == std::wstring::npos ? native.size() : share;
    }
    auto parent = path.parent_path();
    if (!parent.empty() && parent != path && parent.native().size() >= rootLength)
        DirectorySafe(parent);
    path = stone::windows::ExtendedWindowsPath(path);
    auto attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES)
        Require(CreateDirectoryW(path.c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS,
                "Could not create image cache: " + Utf8(path));
    attributes = GetFileAttributesW(path.c_str());
    Require(attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) &&
                !(attributes & FILE_ATTRIBUTE_REPARSE_POINT),
            "Image cache contains an invalid directory or reparse point: " + Utf8(path));
}

void Atomic(const fs::path& value, const std::function<void(std::ostream&)>& write)
{
    auto path = stone::windows::ExtendedWindowsPath(value);
    DirectorySafe(path.parent_path());
    GUID id{};
    Require(SUCCEEDED(CoCreateGuid(&id)), "Could not create image staging identity");
    wchar_t name[40]{};
    StringFromGUID2(id, name, 40);
    auto stage = path.parent_path() / (std::wstring(L".stone-") + name + L".tmp");
    try {
        // The random name is created exclusively before the C++ stream opens it.
        {
            Handle file(
                CreateFileW(stage.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
            Require(file.value != nullptr, "Could not create image staging file");
        }
        {
            std::ofstream output(stage, std::ios::binary | std::ios::trunc);
            output.exceptions(std::ios::badbit | std::ios::failbit);
            write(output);
            output.flush();
            output.close();
        }
        Require(MoveFileExW(stage.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH),
                "Could not publish image file: " + Utf8(path));
    }
    catch (...) {
        DeleteFileW(stage.c_str());
        throw;
    }
}

void ZipEntry(const fs::path& archive, std::string_view name, const fs::path& target)
{
    Image image = OpenFile(archive);
    Require(image.size <= 512ll * 1024 * 1024, "APEX package exceeds size limit");

    struct Archive {
        mz_zip_archive zip{};
        Image image;
        std::exception_ptr failure;

        ~Archive()
        {
            if (zip.m_pState)
                mz_zip_reader_end(&zip);
        }
    } source;

    source.image = image;
    source.zip.m_pIO_opaque = &source;
    source.zip.m_pRead = +[](void* context, mz_uint64 offset, void* bytes, size_t size) -> size_t {
        auto& value = *static_cast<Archive*>(context);
        try {
            Require(offset <= INT64_MAX, "Invalid APEX ZIP offset");
            value.image.Read(static_cast<int64_t>(offset), std::span(static_cast<uint8_t*>(bytes), size));
            return size;
        }
        catch (...) {
            value.failure = std::current_exception();
            return 0;
        }
    };
    Require(mz_zip_reader_init(&source.zip, image.size, 0), "Invalid APEX ZIP archive");
    int index = -1;
    auto count = mz_zip_reader_get_num_files(&source.zip);
    Require(count <= 65536, "APEX ZIP entry count exceeds limit");
    for (mz_uint i = 0; i < count; i++) {
        mz_uint size = mz_zip_reader_get_filename(&source.zip, i, nullptr, 0);
        if (size != name.size() + 1)
            continue;
        std::string filename(size, '\0');
        Require(mz_zip_reader_get_filename(&source.zip, i, filename.data(), size) == size, "Invalid APEX ZIP filename");
        filename.resize(size - 1);
        if (filename != name)
            continue;
        Require(index < 0, "Duplicate APEX ZIP entry");
        index = static_cast<int>(i);
    }
    mz_zip_archive_file_stat entry{};
    Require(index >= 0 && mz_zip_reader_file_stat(&source.zip, index, &entry) && entry.m_is_supported &&
                !entry.m_is_directory && !entry.m_is_encrypted && entry.m_uncomp_size > 0 &&
                entry.m_uncomp_size <= 512ll * 1024 * 1024,
            "Missing, unsupported or oversized APEX ZIP entry: " + std::string(name));
    Atomic(target, [&](std::ostream& output) {
        struct Writer {
            std::ostream& output;
            uint64_t count = 0, limit;
            std::exception_ptr failure;
        } writer{output, 0, entry.m_uncomp_size, {}};
        auto callback = +[](void* context, mz_uint64 offset, const void* data, size_t length) -> size_t {
            auto& value = *static_cast<Writer*>(context);
            try {
                Require(offset == value.count && value.count <= value.limit && length <= value.limit - value.count,
                        "APEX ZIP output exceeds declared size");
                value.output.write(static_cast<const char*>(data), length);
                value.count += length;
                return length;
            }
            catch (...) {
                value.failure = std::current_exception();
                return 0;
            }
        };
        bool success = mz_zip_reader_extract_to_callback(&source.zip, index, callback, &writer, 0) != 0;
        if (source.failure)
            std::rethrow_exception(source.failure);
        if (writer.failure)
            std::rethrow_exception(writer.failure);
        Require(success && writer.count == writer.limit, "APEX ZIP extraction or CRC verification failed");
    });
}

namespace {
fs::path Destination(const fs::path& directory, const std::string& name)
{
    Require(SafeName(name), "Invalid image filename: " + name);
    return directory / Path(name);
}

bool HasFile(const Partition& partition, const std::string& directory, const std::string& name)
{
    auto files = partition.Files(directory);
    return std::find(files.begin(), files.end(), name) != files.end();
}

bool Copy(const Partition& partition, const std::string& directory, const std::string& name, const fs::path& target,
          bool required = true)
{
    Require(SafeName(name), "Invalid image filename: " + name);
    if (!HasFile(partition, directory, name)) {
        Require(!required, "Missing image file: " + directory + "/" + name);
        return false;
    }
    Atomic(target, [&](auto& stream) {
        Require(partition.Copy(directory, name, stream), "Missing image file: " + name);
    });
    return true;
}

void Tree(const Partition& partition, const std::string& directory, const fs::path& target, int depth = 0)
{
    Require(depth <= 16, "Image directory depth exceeds 16");
    DirectorySafe(target);
    std::set<std::wstring> names;
    for (const auto& entry : partition.List(directory)) {
        auto path = Destination(target, entry.name);
        auto folded = Path(entry.name).wstring();
        if (!folded.empty())
            CharUpperBuffW(folded.data(), static_cast<DWORD>(folded.size()));
        Require(names.insert(folded).second, "Colliding image filename: " + entry.name);
        if (entry.type == Partition::Directory)
            Tree(partition, directory + "/" + entry.name, path, depth + 1);
        else if (entry.type == Partition::Regular)
            Atomic(path, [&](auto& output) {
                Require(partition.Copy(directory, entry.name, output), "Missing image file: " + entry.name);
            });
    }
}

Bytes SmallFile(const fs::path& path, size_t limit)
{
    const auto native = stone::windows::ExtendedWindowsPath(path);
    if (!fs::is_regular_file(native))
        return {};
    Require(fs::file_size(native) <= limit, "Image metadata file exceeds size limit");
    std::ifstream input(native, std::ios::binary);
    Require(bool(input), "Could not read image metadata");
    Bytes bytes(std::istreambuf_iterator<char>(input), {});
    Require(!input.bad() && bytes.size() <= limit, "Could not finish reading image metadata");
    return bytes;
}

std::string Trim(std::string value)
{
    auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
        return {};
    return value.substr(first, value.find_last_not_of(" \t\r\n") - first + 1);
}

class Preparation {
    Image image_;
    std::shared_ptr<Partition> system_;
    fs::path cache_, output_;
    std::map<std::string, std::shared_ptr<Partition>> payloads_;
    std::vector<std::shared_ptr<Partition>> apices_;
    std::shared_ptr<Partition> runtime_;

    void WebViewPackages()
    {
        std::vector<std::string> apks;
        std::set<std::wstring> destinations;
        const auto guestPath = [](const fs::path& path) {
            const auto text = path.generic_u8string();
            return std::string(reinterpret_cast<const char*>(text.data()), text.size());
        };
        auto nativeFile = [&](std::string guest, const fs::path& target) {
            std::set<std::string> visited;
            for (;;) {
                guest = guestPath(Path(guest).lexically_normal());
                Require(visited.size() < 32 && visited.insert(guest).second, "Cyclic WebView native library link");
                std::shared_ptr<Partition> partition;
                std::string relative;
                for (const auto* mount : {"system", "product", "system_ext"}) {
                    const auto prefix = std::string("/") + mount + "/";
                    if (!guest.starts_with(prefix))
                        continue;
                    relative = guest.substr(prefix.size());
                    if (std::string_view(mount) == "system")
                        partition = system_;
                    else if (auto image = OpenImagePartition(image_, mount))
                        partition = OpenPartition(*image);
                    else {
                        partition = system_;
                        relative = std::string(mount) + "/" + relative;
                    }
                    break;
                }
                Require(bool(partition), "WebView native library link leaves image partitions");
                const auto path = Path(relative);
                for (const auto& component : path)
                    Require(SafeName(Utf8(component)), "Invalid WebView native library path");
                const auto parent = guestPath(path.parent_path()), name = Utf8(path.filename());
                const auto link = partition->Link(parent, name);
                if (link.empty()) {
                    Copy(*partition, parent, name, target);
                    break;
                }
                guest = link.starts_with('/') ? link : guestPath(Path(guest).parent_path() / Path(link));
            }
        };
        auto collect = [&](const Partition& partition, const std::string& source, const std::string& mount) {
            for (const auto* parent : {"app", "priv-app"}) {
                auto directory = source.empty() ? std::string(parent) : source + "/" + parent;
                for (const auto& entry : partition.List(directory)) {
                    if (entry.type != Partition::Directory)
                        continue;
                    auto lower = entry.name;
                    for (auto& c : lower)
                        if (c >= 'A' && c <= 'Z')
                            c += 'a' - 'A';
                    if (lower.find("webview") == lower.npos && lower.find("trichrome") == lower.npos)
                        continue;
                    Require(SafeName(entry.name), "Invalid WebView package directory");
                    auto packageDirectory = directory + "/" + entry.name;
                    auto target = mount + "/" + parent + "/" + entry.name;
                    for (const auto& name : partition.Files(packageDirectory)) {
                        if (!name.ends_with(".apk"))
                            continue;
                        auto destination = Destination(output_ / Path(target), name);
                        auto folded = destination.wstring();
                        CharUpperBuffW(folded.data(), static_cast<DWORD>(folded.size()));
                        Require(destinations.insert(folded).second, "Colliding WebView APK path");
                        Copy(partition, packageDirectory, name, destination);
                        apks.push_back("/" + target + "/" + name);
                    }
                    size_t count = 0;
                    for (const auto& architecture : partition.List(packageDirectory + "/lib")) {
                        if (architecture.type != Partition::Directory)
                            continue;
                        Require(SafeName(architecture.name), "Invalid WebView native architecture");
                        const auto suffix = "/lib/" + architecture.name;
                        for (const auto& file : partition.List(packageDirectory + suffix)) {
                            if (!file.name.ends_with(".so") ||
                                (file.type != Partition::Regular && file.type != Partition::Symlink))
                                continue;
                            Require(++count <= 128, "Too many WebView native libraries");
                            auto destination = Destination(output_ / Path(target + suffix), file.name);
                            auto folded = destination.wstring();
                            CharUpperBuffW(folded.data(), static_cast<DWORD>(folded.size()));
                            Require(destinations.insert(folded).second, "Colliding WebView native library path");
                            nativeFile("/" + target + suffix + "/" + file.name, destination);
                        }
                    }
                }
            }
        };
        collect(*system_, "", "system");
        for (const auto* name : {"product", "system_ext"}) {
            if (auto image = OpenImagePartition(image_, name))
                collect(*OpenPartition(*image), "", name);
            else
                collect(*system_, name, name);
        }
        std::sort(apks.begin(), apks.end());
        // Publish only after all APK copies succeed. Runtime provider selection
        // must still parse and validate the original manifests and signatures.
        Atomic(output_ / L"system/etc/stone-webview-apks.list", [&](auto& output) {
            for (const auto& apk : apks)
                output << apk << '\n';
        });
    }

    std::shared_ptr<Partition> Payload(const std::string& module)
    {
        if (!ModuleName(module))
            return {};
        if (auto found = payloads_.find(module); found != payloads_.end())
            return found->second;
        auto directory = cache_ / L"apex-payloads";
        DirectorySafe(directory);
        auto package = directory / Path(module + ".apex"), compressed = directory / Path(module + ".capex"),
             payload = directory / Path(module + ".img");

        struct Cleanup {
            fs::path a, b;

            ~Cleanup()
            {
                DeleteFileW(a.c_str());
                DeleteFileW(b.c_str());
            }
        } cleanup{package, compressed};

        if (!Copy(*system_, "apex", module + ".apex", package, false)) {
            if (!Copy(*system_, "apex", module + ".capex", compressed, false))
                return {};
            ZipEntry(compressed, "original_apex", package);
        }
        ZipEntry(package, "apex_payload.img", payload);
        auto partition = OpenPartition(OpenFile(payload));
        payloads_[module] = partition;
        return partition;
    }

    std::optional<Bytes> LibraryFrom(const Partition& partition, const std::string& directory, const std::string& name)
    {
        if (!HasFile(partition, directory, name))
            return {};
        return partition.Read(directory, name);
    }

    std::optional<Bytes> Library(const std::string& name)
    {
        if ((name == "libc.so" || name == "libm.so" || name == "libdl.so") && runtime_)
            return LibraryFrom(*runtime_, "lib64/bionic", name);
        for (const auto& apex : apices_) {
            if (auto bytes = LibraryFrom(*apex, "lib64", name))
                return bytes;
            if (name == "libart.so")
                return {};
        }
        return LibraryFrom(*system_, "lib64", name);
    }

public:
    Preparation(const fs::path& image, fs::path cache)
        : image_(OpenFile(image)), system_(OpenPartition(OpenSystem(image_))), cache_(std::move(cache)),
          output_(cache_ / L"art-runtime")
    {
    }

    fs::path Run(const std::vector<std::string>& extras)
    {
        DirectorySafe(output_ / L"lib64");
        DirectorySafe(output_ / L"data");
        auto art = Payload("com.android.art");
        if (!art)
            art = Payload("com.google.android.art");
        Require(bool(art), "The system image does not contain an ART APEX");
        apices_.push_back(art);
        for (const auto* module :
             {"com.android.i18n", "com.android.tzdata", "com.google.android.tzdata", "com.android.conscrypt",
              "com.google.android.conscrypt", "com.android.os.statsd", "com.google.android.os.statsd"})
            if (auto payload = Payload(module))
                if (std::find(apices_.begin(), apices_.end(), payload) == apices_.end())
                    apices_.push_back(payload);
        auto packages = system_->Files("apex");
        std::sort(packages.begin(), packages.end());
        for (const auto& name : packages) {
            if (!name.ends_with(".apex") && !name.ends_with(".capex"))
                continue;
            auto module = name.substr(0, name.rfind('.'));
            auto payload = Payload(module);
            if (!payload || std::find(apices_.begin(), apices_.end(), payload) != apices_.end())
                continue;
            if (std::regex_match(module, std::regex("com\\.(google\\.)?android\\.tzdata[0-9]*")) ||
                HasFile(*payload, "etc/classpaths", "bootclasspath.pb"))
                apices_.push_back(payload);
        }
        runtime_ = Payload("com.android.runtime");
        for (const auto* directory : {"etc", "usr/keychars", "usr/keylayout", "fonts"})
            Tree(*system_, directory, output_ / L"system" / Path(directory));
        for (const auto& entry : system_->List("fonts"))
            if (entry.type == Partition::Symlink) {
                auto target = Destination(output_ / L"system/fonts", entry.name);
                auto name = entry.name;
                std::set<std::string> seen;
                while (true) {
                    Require(seen.size() < 32 && seen.insert(name).second, "Cyclic image font alias: " + entry.name);
                    auto next = system_->Link("fonts", name);
                    if (next.empty())
                        break;
                    if (next.starts_with("/system/fonts/"))
                        next = next.substr(14);
                    Require(SafeName(next), "Font alias escapes the system font directory");
                    name = next;
                }
                Copy(*system_, "fonts", name, target);
            }
        Copy(*system_, "", "build.prop", output_ / L"system/build.prop");
        for (const auto& name : system_->Files("framework"))
            if (name.ends_with(".jar") || name == "framework-res.apk")
                Copy(*system_, "framework", name, Destination(output_ / L"system/framework", name));
        std::map<std::string, fs::path> fragments;
        for (const auto& apex : apices_) {
            auto module = ApexName(apex->Read("", "apex_manifest.pb"));
            auto directory = output_ / L"apex" / Path(module);
            Require(!fragments.contains("/apex/" + module + "/"), "Duplicate canonical APEX name");
            for (const auto* child : {"lib64", "javalib", "etc", "cacerts"})
                Tree(*apex, child, directory / child);
            for (const auto& name : apex->Files("lib64"))
                Copy(*apex, "lib64", name, Destination(output_ / L"lib64", name));
            fragments["/apex/" + module + "/"] = directory / L"etc/classpaths/bootclasspath.pb";
        }
        Properties properties;
        auto bytes = SmallFile(output_ / L"system/build.prop", 4 * 1024 * 1024);
        std::istringstream lines(Text(bytes));
        for (std::string line; std::getline(lines, line);) {
            line = Trim(line);
            if (line.empty() || line[0] == '#' || line[0] == '!')
                continue;
            auto equals = line.find('=');
            if (equals != std::string::npos && equals > 0)
                properties[Trim(line.substr(0, equals))] = Trim(line.substr(equals + 1));
        }
        std::vector<std::string> boot;
        std::set<std::string> unique;
        auto fragment = [&](const fs::path& path, const std::string& prefix) {
            for (const auto& jar : BootClassPath(SmallFile(path, 1024 * 1024), prefix, properties))
                if (unique.insert(jar).second)
                    boot.push_back(jar);
        };
        auto artFragment = fragments.find("/apex/com.android.art/");
        Require(artFragment != fragments.end(), "Missing ART boot class path fragment");
        fragment(artFragment->second, artFragment->first);
        fragments.erase(artFragment);
        fragment(output_ / L"system/etc/classpaths/bootclasspath.pb", "");
        for (const auto& [prefix, path] : fragments)
            fragment(path, prefix);
        Require(!boot.empty(), "Image has no boot class path");
        for (const auto& jar : boot)
            Require(fs::is_regular_file(stone::windows::ExtendedWindowsPath(output_ / Path(jar.substr(1)))),
                    "Missing image boot JAR: " + jar);
        Atomic(output_ / L"system/etc/stone-bootclasspath.list", [&](auto& output) {
            for (const auto& jar : boot)
                output << jar << '\n';
        });
        auto compiler = art->Read("bin", "dex2oat64");
        Require(!compiler.empty(), "Missing image ART compiler: bin/dex2oat64");
        Atomic(output_ / L"bin/dex2oat64", [&](auto& stream) {
            stream.write(reinterpret_cast<const char*>(compiler.data()), compiler.size());
        });
        std::set<std::string> roots{"libart.so",     "libartpalette-system.so", "libjavacore.so",    "libopenjdkjvm.so",
                                    "libopenjdk.so", "libjavacrypto.so",        "libjnigraphics.so", "libandroid.so"};
        if (Library("libwebviewchromium_plat_support.so"))
            roots.insert("libwebviewchromium_plat_support.so");
        roots.insert(extras.begin(), extras.end());
        std::deque<std::string> pending(roots.begin(), roots.end());
        for (const auto& name : ElfDependencies(compiler))
            pending.push_back(name);
        std::set<std::string> seen;
        while (!pending.empty()) {
            auto name = pending.front();
            pending.pop_front();
            if (!seen.insert(name).second)
                continue;
            Require(seen.size() <= 1024 && SafeName(name) &&
                        std::regex_match(name, std::regex("[A-Za-z0-9_+@.-]+\\.so")),
                    "Invalid ART dependency name/count: " + name);
            auto library = Library(name);
            if (!library) {
                Require(!roots.contains(name), "Missing ART library: " + name);
                continue;
            }
            Atomic(Destination(output_ / L"lib64", name), [&](auto& stream) {
                stream.write(reinterpret_cast<const char*>(library->data()), library->size());
            });
            for (const auto& dependency : ElfDependencies(*library))
                pending.push_back(dependency);
        }
        WebViewPackages();
        return output_;
    }
};
}

fs::path Prepare(const fs::path& image, const fs::path& directory, const std::vector<std::string>& extras)
{
    auto cache = stone::windows::ExtendedWindowsPath(directory);
    DirectorySafe(cache);
    Handle worker(CreateFileW((cache / L".prepare-worker.lock").c_str(), GENERIC_READ | GENERIC_WRITE,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                              nullptr));
    OVERLAPPED lock{};
    Require(worker.value && LockFileEx(worker.value, LOCKFILE_EXCLUSIVE_LOCK, 0, 1, 0, &lock),
            "Could not lock image preparation cache");
    return Preparation(image, cache).Run(extras);
}
}

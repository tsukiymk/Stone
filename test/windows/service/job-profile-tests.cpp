#include "JobProfiles.h"
#include "../../../src/windows/host/WindowsPath.h"
#include <windows.h>
#include <cstdio>
#include <fstream>
#include <stdexcept>
using namespace stone::windows;
namespace fs = std::filesystem;

namespace {
void Check(bool value, const char* message)
{
    if (!value)
        throw std::runtime_error(message);
}

template <class Work> void Rejected(Work work, const char* message)
{
    bool rejected = false;
    try {
        work();
    }
    catch (const std::exception&) {
        rejected = true;
    }
    Check(rejected, message);
}
}

int wmain()
{
    try {
        auto root = fs::path(STONE_PROFILE_TEST_DIRECTORY) /
                    (std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
        auto data = root / L"profile", prepared = root / L"prepared", image = root / L"system.img",
             apk = root / L"staged.apk", file = root / L"catalog.bin";
        fs::create_directories(data / L"data");
        fs::create_directories(prepared / L"system");
        {
            std::ofstream out(image);
            out << "image fixture";
        }
        {
            std::ofstream out(apk);
            out << "immutable APK fixture";
        }
        std::vector<std::wstring> fields{data.wstring(),
                                         image.wstring(),
                                         prepared.wstring(),
                                         apk.wstring(),
                                         (root / L"removed-download.apk").wstring(),
                                         L"com.example.jobs",
                                         JobImageStamp(image),
                                         L""};
        JobProfiles store(file);
        auto profile = store.Register(fields);
        Check(profile.token.size() == 32, "Missing launch authorization");
        auto code = StageArtLaunchCode(apk, prepared, ".apk");
        auto staged = prepared / fs::path(code.substr(1));
        Check(code == "/stone/code/" + std::string(profile.apkDigest.begin(), profile.apkDigest.end()) + ".apk",
              "Launch staging differs from verified code identity");
        Check((GetFileAttributesW(ExtendedWindowsPath(staged).c_str()) & FILE_ATTRIBUTE_READONLY) != 0 &&
                  StageArtLaunchCode(apk, prepared, ".apk") == code,
              "Staged code was mutable or not reused");
        Rejected(
            [&] {
                StageArtLaunchCode(apk, prepared, "/bad");
            },
            "Invalid code extension accepted");
        Check(store.Register(fields).token == profile.token, "Unchanged registration invalidated queued launches");
        JobProfiles restored(file);
        Check(restored.All().at(profile.Key()).Fields() == profile.Fields(),
              "Profile catalog did not recover exact identity");
        auto wake = fields;
        wake[7] = profile.token;
        Check(restored.Register(wake).token == profile.token, "Valid cold wake rejected");
        auto temporary = file;
        temporary += L".tmp";
        fs::create_directory(temporary);
        Rejected(
            [&] {
                restored.Remove(data / L"data", fields[5]);
            },
            "Failed catalog publication accepted removal");
        Check(restored.All().size() == 1, "Failed catalog publication changed in-memory authorization");
        fs::remove(temporary);
        restored.Remove(data / L"data", fields[5]);
        Check(JobProfiles(file).All().empty(), "Removed launch authorization survived restart");
        Rejected(
            [&] {
                restored.Register(wake);
            },
            "Uninstalled launch token recreated its profile");
        auto replacement = restored.Register(fields);
        Check(replacement.token != profile.token, "Reinstallation reused old launch authorization");
        Rejected(
            [&] {
                restored.Register(wake);
            },
            "Old launch token authorized replacement installation");
        {
            std::ofstream out(apk, std::ios::app);
            out << "changed";
        }
        Rejected(
            [&] {
                replacement.ValidateForLaunch();
            },
            "Changed APK bytes were accepted for old jobs");
        {
            std::ofstream out(apk);
            out << "immutable APK fixture";
        }
        replacement.ValidateForLaunch();
        {
            std::ofstream out(image, std::ios::app);
            out << "changed";
        }
        Rejected(
            [&] {
                replacement.ValidateForLaunch();
            },
            "Changed selected image was accepted");
        fields[6] = JobImageStamp(image);
        replacement = restored.Register(fields);
        auto other = fields;
        other[5] = L"com.other.jobs";
        restored.Register(other);
        other = fields;
        other[0] = (root / L"other-profile").wstring();
        fs::create_directories(fs::path(other[0]) / L"data");
        restored.Register(other);
        restored.Remove(data / L"data", fields[5]);
        Check(restored.All().size() == 2, "Removal escaped package or data directory");
        Check(JobProfiles(file).All().size() == 2, "Unrelated profiles were not preserved on disk");
        auto longDirectory = prepared;
        while ((longDirectory / L"long.apk").wstring().size() <= MAX_PATH) {
            longDirectory /= std::wstring(80, L'a');
        }
        fs::create_directories(ExtendedWindowsPath(longDirectory));
        auto longApk = longDirectory / L"long.apk";
        Check(longApk.wstring().size() > 260, "Long-path fixture is too short");
        {
            std::ofstream output(ExtendedWindowsPath(longApk));
            output << "long APK fixture";
        }
        auto longFields = fields;
        longFields[3] = longApk.wstring();
        longFields[5] = L"com.example.long";
        auto longProfile = restored.Register(longFields);
        longProfile.ValidateForLaunch();
        JobProfiles recoveredLong(file);
        recoveredLong.All().at(longProfile.Key()).ValidateForLaunch();
        std::puts(
            "PASS cold background profiles, exact code/image identity, revocable launch tokens, atomic rollback and package/data isolation");
        return 0;
    }
    catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL job profiles: %s\n", error.what());
        return 1;
    }
}

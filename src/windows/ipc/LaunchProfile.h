#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace stone::windows {
// Immutable, content-addressed input shared by normal and diagnostic launches.
std::string StageArtLaunchCode(const std::filesystem::path& source, const std::filesystem::path& root,
                               std::string_view extension);

struct JobProfile {
    std::filesystem::path data, image, prepared, apk, source;
    std::wstring package, imageStamp, apkDigest, token;
    std::wstring Key() const;
    std::vector<std::wstring> Fields() const;
    void ValidateForLaunch() const;
};

std::wstring JobImageStamp(const std::filesystem::path& image);
JobProfile DecodeJobProfile(const std::vector<std::wstring>& fields);
std::wstring LaunchCodeDigest(const std::filesystem::path& file);
}

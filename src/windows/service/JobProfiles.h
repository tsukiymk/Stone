#pragma once

#include "../ipc/LaunchProfile.h"
#include <map>

namespace stone::windows {
std::filesystem::path JobProfileStorePath(const std::filesystem::path& directory);

class JobProfiles {
    std::filesystem::path file_;
    std::map<std::wstring, JobProfile> profiles_;
    void Save(const std::map<std::wstring, JobProfile>& profiles) const;

public:
    explicit JobProfiles(std::filesystem::path file);
    JobProfile Register(const std::vector<std::wstring>& fields);
    void Remove(const std::filesystem::path& androidData, const std::wstring& package);

    const std::map<std::wstring, JobProfile>& All() const
    {
        return profiles_;
    }
};
}

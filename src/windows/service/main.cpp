#include "Broker.h"
#include "ServiceHost.h"
#include "../host/ProcessLog.h"
#include <windows.h>
#include <shellapi.h>
#include <cstdio>
#include <stdexcept>

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    int count = 0;
    auto arguments = CommandLineToArgvW(GetCommandLineW(), &count);
    int result = 1;
    try {
        std::wstring name(32768, L'\0');
        DWORD length = GetModuleFileNameW(nullptr, name.data(), static_cast<DWORD>(name.size()));
        if (!arguments || !length || length >= name.size())
            throw std::runtime_error("Cannot locate StoneService");
        auto directory = std::filesystem::path(name.substr(0, length)).parent_path();
        std::vector<std::wstring> values;
        for (int index = 1; index < count; ++index)
            values.emplace_back(arguments[index]);
        if (values.empty() || values == std::vector<std::wstring>{L"--service"})
            result = stone::windows::RunWindowsService(directory);
        else {
            stone::windows::StartProcessLog();
            result = stone::windows::RunService(values, directory);
        }
    }
    catch (const std::exception& error) {
        std::fprintf(stderr, "StoneService: %s\n", error.what());
    }
    if (arguments)
        LocalFree(arguments);
    stone::windows::StopProcessLog();
    return result;
}

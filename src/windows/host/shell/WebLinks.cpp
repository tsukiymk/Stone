#include "WebLinks.h"
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.System.h>
#include <cstdio>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>

namespace stone::windows {
namespace {
using winrt::Windows::Foundation::Uri;
using winrt::Windows::Foundation::IAsyncOperation;

struct Apartment {
    Apartment()
    {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
    }

    ~Apartment()
    {
        winrt::uninit_apartment();
    }
};

Uri WebUri(const std::wstring& url)
{
    if (url.empty() || url.size() > 8192 || url.find_first_of(L"\\\"<>") != url.npos)
        throw std::invalid_argument("Invalid web URL");
    for (auto ch : url)
        if (ch <= L' ' || ch == 0x7f)
            throw std::invalid_argument("Invalid web URL");
    // Check the original scheme as well as the parsed URI. No file paths,
    // custom Windows protocols, credentials, or command lines reach Launcher.
    auto scheme = url.substr(0, url.find(L':'));
    for (auto& ch : scheme)
        if (ch >= L'A' && ch <= L'Z')
            ch += L'a' - L'A';
    if ((scheme != L"http" && scheme != L"https") || url.substr(scheme.size(), 3) != L"://")
        throw std::invalid_argument("Invalid web URL");
    const auto start = scheme.size() + 3;
    if (start == url.size() || url.find_first_of(L"/?#", start) == start)
        throw std::invalid_argument("Invalid web URL");
    const auto end = url.find_first_of(L"/?#", start);
    if (url.substr(start, end == url.npos ? url.size() - start : end - start).find(L'@') != url.npos)
        throw std::invalid_argument("Invalid web URL");
    static thread_local Apartment apartment;
    Uri parsed(url);
    if ((parsed.SchemeName() != L"http" && parsed.SchemeName() != L"https") || parsed.Host().empty() ||
        !parsed.UserName().empty() || !parsed.Password().empty())
        throw std::invalid_argument("Invalid web URL");
    return parsed;
}

struct Pending {
    std::mutex mutex;
    uint64_t next = 0;
    std::map<uint64_t, IAsyncOperation<bool>> requests;
};
}

void ValidateWebUrl(const std::wstring& url)
{
    try {
        (void)WebUri(url);
    }
    catch (const winrt::hresult_error&) {
        throw std::invalid_argument("Invalid web URL");
    }
}
void OpenServiceWebUrl(const std::wstring& url)
try {
    auto uri = WebUri(url);
    static auto pending = std::make_shared<Pending>();
    auto state = pending;
    uint64_t id;
    {
        std::lock_guard lock(state->mutex);
        if (state->requests.size() >= 16)
            throw std::runtime_error("Browser activation queue is full");
        id = ++state->next;
        state->requests.emplace(id, nullptr);
    }
    try {
        auto operation = winrt::Windows::System::Launcher::LaunchUriAsync(uri);
        {
            std::lock_guard lock(state->mutex);
            state->requests.at(id) = operation;
        }
        operation.Completed([state, id](const auto& result, auto) noexcept {
            try {
                if (!result.GetResults())
                    std::fprintf(stderr, "[browser] Windows declined web link activation\n");
            }
            catch (...) {
                std::fprintf(stderr, "[browser] Windows web link activation failed\n");
            }
            std::lock_guard lock(state->mutex);
            state->requests.erase(id);
        });
    }
    catch (...) {
        std::lock_guard lock(state->mutex);
        state->requests.erase(id);
        throw;
    }
}
catch (const winrt::hresult_error&) {
    throw std::runtime_error("Windows web link activation failed");
}
}

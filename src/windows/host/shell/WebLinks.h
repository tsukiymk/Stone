#pragma once

#include <string>

namespace stone::windows {
void ValidateWebUrl(const std::wstring& url);
// Queue one Windows default-browser activation, without waiting for page loading.
void OpenServiceWebUrl(const std::wstring& url);
}

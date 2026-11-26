#pragma once

#include <string>
#include <string_view>

namespace stone::session {
inline bool IsPackageName(std::wstring_view name)
{
    if (name.empty() || name.size() > 255)
        return false;
    bool first = true, dot = false;
    for (wchar_t ch : name) {
        if (ch == L'.') {
            if (first)
                return false;
            first = true;
            dot = true;
            continue;
        }
        bool letter = (ch >= L'A' && ch <= L'Z') || (ch >= L'a' && ch <= L'z');
        if (first ? !letter : !letter && !(ch >= L'0' && ch <= L'9') && ch != L'_')
            return false;
        first = false;
    }
    return dot && !first;
}

inline std::wstring Quote(const std::wstring& value)
{
    std::wstring result = L"\"";
    size_t slashes = 0;
    for (wchar_t ch : value) {
        if (ch == L'\\') {
            ++slashes;
            continue;
        }
        result.append(slashes * (ch == L'\"' ? 2 : 1), L'\\');
        slashes = 0;
        if (ch == L'\"')
            result += L'\\';
        result += ch;
    }
    result.append(slashes * 2, L'\\');
    return result + L'\"';
}
}

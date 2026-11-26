#include "../../../../src/windows/host/shell/ShellIconUpload.h"
#include <algorithm>
#include <cstdio>
#include <functional>
#include <stdexcept>
using stone::windows::ShellIconUpload;

void Require(bool value)
{
    if (!value)
        throw std::runtime_error("Icon upload check failed");
}

void Rejected(const std::function<void()>& work)
{
    try {
        work();
    }
    catch (const std::invalid_argument&) {
        return;
    }
    throw std::runtime_error("Invalid icon upload was accepted");
}

int main()
{
    try {
        ShellIconUpload upload;
        Rejected([&] {
            upload.Begin(0);
        });
        Rejected([&] {
            upload.Begin(1024 * 1024 + 1);
        });
        Rejected([&] {
            upload.Append(0, L"ff");
        });
        Rejected([&] {
            upload.Complete();
        });
        upload.Begin(4);
        upload.Append(0, L"89ab");
        Rejected([&] {
            upload.Append(0, L"cdef");
        });
        Rejected([&] {
            upload.Append(3, L"cd");
        });
        Rejected([&] {
            upload.Append(2, L"c");
        });
        Rejected([&] {
            upload.Append(2, L"cdzz");
        });
        Rejected([&] {
            upload.Append(2, L"cdef00");
        });
        Rejected([&] {
            upload.Complete();
        });
        upload.Append(2, L"cdef");
        Require(upload.Complete() == std::vector<uint8_t>({0x89, 0xab, 0xcd, 0xef}));
        Rejected([&] {
            upload.Complete();
        });
        Rejected([&] {
            upload.Append(4, L"ff");
        });
        ShellIconUpload other;
        other.Begin(1);
        other.Append(0, L"42");
        upload.Begin(1024 * 1024);
        size_t offset = 0;
        while (offset < 1024 * 1024) {
            auto count = std::min<size_t>(8000, 1024 * 1024 - offset);
            upload.Append(offset, std::wstring(count * 2, L'f'));
            offset += count;
        }
        auto full = upload.Complete();
        Require(full.size() == 1024 * 1024 && full.front() == 255 && full.back() == 255);
        Require(other.Complete() == std::vector<uint8_t>({0x42}));
        upload.Begin(2);
        upload.Append(0, L"ff");
        upload.Begin(1);
        upload.Append(0, L"01");
        Require(upload.Complete() == std::vector<uint8_t>({1}));
        std::puts(
            "PASS ordered icon chunks, atomic invalid-chunk rejection, completeness, bounded full-size upload and Client isolation.");
        return 0;
    }
    catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}

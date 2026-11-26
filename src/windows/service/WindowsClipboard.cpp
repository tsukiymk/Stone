#include "WindowsClipboard.h"
#include "../ipc/ServiceProtocol.h"
#include <windows.h>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <deque>
#include <future>
#include <mutex>
#include <thread>

namespace stone::windows {
namespace {
using ipc::Check;

struct Memory {
    HGLOBAL value;

    explicit Memory(size_t bytes) : value(GlobalAlloc(GMEM_MOVEABLE, bytes))
    {
        Check(value != nullptr, "Cannot allocate clipboard data");
    }

    ~Memory()
    {
        if (value)
            GlobalFree(value);
    }

    void Publish(UINT format)
    {
        Check(SetClipboardData(format, value) != nullptr, "Cannot publish clipboard data");
        value = nullptr;
    }
};

struct Locked {
    HGLOBAL handle;
    void* data;

    explicit Locked(HGLOBAL value) : handle(value), data(GlobalLock(value))
    {
        Check(data != nullptr, "Cannot lock clipboard data");
    }

    ~Locked()
    {
        GlobalUnlock(handle);
    }
};

struct Open {
    explicit Open(HWND window)
    {
        auto deadline = GetTickCount64() + 50;
        for (;;) {
            if (OpenClipboard(window))
                return;
            auto error = GetLastError();
            if (error != ERROR_ACCESS_DENIED || GetTickCount64() >= deadline)
                throw std::runtime_error("Windows clipboard is busy or unavailable (" + std::to_string(error) + ")");
            Sleep(5);
        }
    }

    ~Open()
    {
        CloseClipboard();
    }
};

constexpr uint32_t MetadataMagic = 0x31434353;

struct Metadata {
    uint32_t magic, label, extras;
};

void Validate(const std::vector<std::wstring>& fields)
{
    Check(!fields.empty(), "Missing clipboard operation");
    if (fields[0] != L"set") {
        Check(fields.size() == 1 && (fields[0] == L"sequence" || fields[0] == L"describe" || fields[0] == L"get" ||
                                     fields[0] == L"clear"),
              "Invalid clipboard operation");
        return;
    }
    Check(fields.size() == 4 && fields[1].size() <= 1024 && fields[2].size() <= 8192 && fields[3].size() <= 8192,
          "Invalid clipboard data size");
    for (const auto& field : fields)
        Check(field.find(L'\0') == field.npos, "Clipboard data contains a null character");
    Check(fields[3].size() % 4 == 0 &&
              fields[3].find_first_not_of(L"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/=") ==
                  fields[3].npos,
          "Invalid clipboard metadata");
}

std::vector<std::wstring> Perform(HWND window, UINT format, const std::vector<std::wstring>& fields,
                                  std::vector<std::wstring>& cached)
{
    const auto& operation = fields[0];
    if (operation == L"sequence")
        return {std::to_wstring(GetClipboardSequenceNumber())};
    if (operation == L"describe") {
        auto sequence = std::to_wstring(GetClipboardSequenceNumber());
        if (!cached.empty() && cached[0] == sequence) {
            auto result = cached;
            result[4].clear();
            return result;
        }
        // Availability queries must not trigger a foreign app's delayed renderer.
        const auto present = IsClipboardFormatAvailable(CF_UNICODETEXT) ? L"1" : L"0";
        return {sequence, present, present, L"Windows", L"", L""};
    }
    if (operation == L"set") {
        Memory text((fields[2].size() + 1) * sizeof(wchar_t));
        {
            Locked lock(text.value);
            std::memcpy(lock.data, fields[2].c_str(), (fields[2].size() + 1) * sizeof(wchar_t));
        }
        Metadata header{MetadataMagic, static_cast<uint32_t>(fields[1].size()),
                        static_cast<uint32_t>(fields[3].size())};
        Memory metadata(sizeof(header) + (fields[1].size() + fields[3].size()) * sizeof(wchar_t));
        {
            Locked lock(metadata.value);
            auto bytes = static_cast<unsigned char*>(lock.data);
            std::memcpy(bytes, &header, sizeof(header));
            std::memcpy(bytes + sizeof(header), fields[1].data(), fields[1].size() * sizeof(wchar_t));
            std::memcpy(bytes + sizeof(header) + fields[1].size() * sizeof(wchar_t), fields[3].data(),
                        fields[3].size() * sizeof(wchar_t));
        }
        Open open(window);
        Check(EmptyClipboard() != FALSE, "Cannot empty Windows clipboard");
        text.Publish(CF_UNICODETEXT);
        metadata.Publish(format);
        return {std::to_wstring(GetClipboardSequenceNumber()),
                L"1",
                fields[2].empty() ? L"0" : L"1",
                fields[1],
                fields[2],
                fields[3]};
    }
    Open open(window);
    if (operation == L"clear") {
        Check(EmptyClipboard() != FALSE, "Cannot clear Windows clipboard");
        return {std::to_wstring(GetClipboardSequenceNumber()), L"0", L"0", L"", L"", L""};
    }
    std::vector<std::wstring> result{std::to_wstring(GetClipboardSequenceNumber()), L"0", L"0", L"", L"", L""};
    if (!IsClipboardFormatAvailable(CF_UNICODETEXT))
        return result;
    auto data = static_cast<HGLOBAL>(GetClipboardData(CF_UNICODETEXT));
    Check(data != nullptr, "Cannot read Windows clipboard text");
    auto bytes = GlobalSize(data);
    Check(bytes >= sizeof(wchar_t), "Invalid Windows clipboard text allocation");
    {
        Locked lock(data);
        auto start = static_cast<const wchar_t*>(lock.data),
             end = start + std::min<size_t>(bytes / sizeof(wchar_t), 8193);
        auto stop = std::find(start, end, L'\0');
        Check(stop != end && stop - start <= 8192, "Invalid Windows clipboard text");
        result[4].assign(start, stop);
    }
    result[1] = L"1";
    result[2] = result[4].empty() ? L"0" : L"1";
    result[3] = L"Windows";
    if (IsClipboardFormatAvailable(format)) {
        auto custom = static_cast<HGLOBAL>(GetClipboardData(format));
        if (custom && GlobalSize(custom) >= sizeof(Metadata) && GlobalSize(custom) <= 32768) {
            Locked lock(custom);
            Metadata header{};
            std::memcpy(&header, lock.data, sizeof(header));
            if (header.magic == MetadataMagic && header.label <= 1024 && header.extras <= 8192 &&
                sizeof(header) + (header.label + header.extras) * sizeof(wchar_t) <= GlobalSize(custom)) {
                auto label =
                    reinterpret_cast<const wchar_t*>(static_cast<const unsigned char*>(lock.data) + sizeof(header));
                std::wstring name(label, header.label), extras(label + header.label, header.extras);
                if (name.find(L'\0') == name.npos && extras.find(L'\0') == extras.npos) {
                    result[3] = std::move(name);
                    result[5] = std::move(extras);
                }
            }
        }
    }
    // Delayed rendering can change the sequence while GetClipboardData runs.
    result[0] = std::to_wstring(GetClipboardSequenceNumber());
    return result;
}
}

struct WindowsClipboard::State {
    struct Task {
        std::vector<std::wstring> fields;
        std::promise<std::vector<std::wstring>> result;
        bool cancelled = false;
    };

    std::mutex mutex;
    ipc::Handle wake{CreateEventW(nullptr, FALSE, FALSE, nullptr)};
    std::deque<std::shared_ptr<Task>> tasks;
    bool stopping = false;
    std::promise<void> ready, closed;
    std::shared_future<void> done = closed.get_future().share();

    void Run(HDESK desktop) noexcept
    {
        HWND window = nullptr;
        try {
            Check(SetThreadDesktop(desktop) != FALSE, "Cannot select clipboard worker desktop");
            window = CreateWindowExW(0, L"STATIC", L"Stone clipboard", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr,
                                     GetModuleHandleW(nullptr), nullptr);
            Check(window != nullptr, "Cannot create clipboard owner window");
            auto format = RegisterClipboardFormatW(L"Stone.Android.Clipboard.Description.v1");
            Check(format != 0, "Cannot register clipboard metadata");
            ready.set_value();
            std::vector<std::wstring> cached;
            for (;;) {
                Check(MsgWaitForMultipleObjects(1, &wake.value, FALSE, INFINITE, QS_ALLINPUT) != WAIT_FAILED,
                      "Clipboard worker wait failed");
                MSG message{};
                while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                    TranslateMessage(&message);
                    DispatchMessageW(&message);
                }
                std::shared_ptr<Task> task;
                {
                    std::lock_guard lock(mutex);
                    if (stopping)
                        break;
                    if (tasks.empty())
                        continue;
                    task = tasks.front();
                    tasks.pop_front();
                    if (!tasks.empty())
                        SetEvent(wake.value);
                    if (task->cancelled)
                        continue;
                }
                try {
                    auto result = Perform(window, format, task->fields, cached);
                    if (task->fields[0] == L"get" || task->fields[0] == L"set" || task->fields[0] == L"clear")
                        cached = result;
                    task->result.set_value(std::move(result));
                }
                catch (...) {
                    task->result.set_exception(std::current_exception());
                }
            }
        }
        catch (...) {
            try {
                ready.set_exception(std::current_exception());
            }
            catch (...) {
            }
        }
        if (window)
            DestroyWindow(window);
        {
            std::lock_guard lock(mutex);
            stopping = true;
            tasks.clear();
        }
        closed.set_value();
    }
};

WindowsClipboard::WindowsClipboard() : state_(std::make_shared<State>())
{
    Check(state_->wake.value != nullptr, "Cannot create clipboard worker event");
    auto ready = state_->ready.get_future();
    auto desktop = GetThreadDesktop(GetCurrentThreadId());
    std::thread([state = state_, desktop] {
        state->Run(desktop);
    }).detach();
    try {
        Check(ready.wait_for(std::chrono::seconds(2)) == std::future_status::ready,
              "Clipboard worker startup timed out");
        ready.get();
    }
    catch (...) {
        Stop();
        throw;
    }
}

WindowsClipboard::~WindowsClipboard()
{
    Stop();
}

bool WindowsClipboard::Stop(unsigned timeoutMillis) noexcept
{
    {
        std::lock_guard lock(state_->mutex);
        state_->stopping = true;
    }
    SetEvent(state_->wake.value);
    return state_->done.wait_for(std::chrono::milliseconds(timeoutMillis)) == std::future_status::ready;
}

std::vector<std::wstring> WindowsClipboard::Exchange(const std::vector<std::wstring>& fields)
{
    Validate(fields);
    auto task = std::make_shared<State::Task>();
    task->fields = fields;
    auto result = task->result.get_future();
    {
        std::lock_guard lock(state_->mutex);
        Check(!state_->stopping && state_->tasks.size() < 32, "Clipboard worker is unavailable");
        state_->tasks.push_back(task);
    }
    SetEvent(state_->wake.value);
    if (result.wait_for(std::chrono::milliseconds(250)) != std::future_status::ready) {
        std::lock_guard lock(state_->mutex);
        task->cancelled = true;
        throw std::runtime_error("Windows clipboard operation timed out; result may be uncertain");
    }
    return result.get();
}
}

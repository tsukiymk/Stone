#include "WindowsNotifications.h"
#include "../ipc/NotificationIdentity.h"
#include <windows.h>
#include <objbase.h>
#include <bcrypt.h>
#include <sddl.h>
#include <winrt/Windows.Data.Xml.Dom.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.UI.Notifications.h>
#include <array>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <cstdio>
#include <deque>
#include <exception>
#include <map>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace stone::windows {
namespace fs = std::filesystem;
using namespace winrt::Windows::UI::Notifications;

struct WindowsNotifications::State : std::enable_shared_from_this<State> {
    struct Handle {
        HANDLE value;

        explicit Handle(HANDLE handle) : value(handle == INVALID_HANDLE_VALUE ? nullptr : handle)
        {
        }

        ~Handle()
        {
            if (value)
                CloseHandle(value);
        }
    };

    std::mutex mutex;
    fs::path settingsExecutable, diagnosticExecutable;
    std::wstring appId, group, protocol, pipeName;
    ToastNotifier notifier{nullptr};
    Handle stop{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    Handle recipient{nullptr};
    uint64_t nextTicket = 0;
    int previousSetting = -1;

    struct Record {
        uint64_t ticket;
        size_t actions;
        ToastNotification toast{nullptr};
        std::shared_ptr<Handle> receiver;
        std::wstring route;
    };

    std::map<std::wstring, Record> records;
    std::deque<int64_t> events;
    std::deque<std::wstring> routeEvents;

    void Check(bool value, const char* message)
    {
        if (!value)
            throw std::runtime_error(message);
    }

    void Apartment()
    {
        static thread_local bool initialized = false;
        if (!initialized) {
            winrt::init_apartment(winrt::apartment_type::multi_threaded);
            initialized = true;
        }
    }

    std::wstring Hash(const std::wstring& text)
    {
        std::array<unsigned char, 32> bytes{};
        winrt::check_nt(BCryptHash(
            BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(text.data())),
            static_cast<ULONG>(text.size() * sizeof(wchar_t)), bytes.data(), static_cast<ULONG>(bytes.size())));
        std::wstring result;
        for (auto value : bytes) {
            result += L"0123456789abcdef"[value >> 4];
            result += L"0123456789abcdef"[value & 15];
        }
        return result;
    }

    std::wstring Xml(const std::wstring& text)
    {
        std::wstring result;
        for (auto ch : text)
            switch (ch) {
            case L'&':
                result += L"&amp;";
                break;
            case L'<':
                result += L"&lt;";
                break;
            case L'>':
                result += L"&gt;";
                break;
            case L'\"':
                result += L"&quot;";
                break;
            case L'\'':
                result += L"&apos;";
                break;
            default:
                if (ch >= 0x20 || ch == L'\n' || ch == L'\r' || ch == L'\t')
                    result += ch;
            }
        return result;
    }

    bool Transfer(HANDLE pipe, void* data, DWORD length, bool write)
    {
        auto* bytes = static_cast<unsigned char*>(data);
        while (length) {
            Handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
            if (!event.value)
                return false;
            OVERLAPPED overlap{};
            overlap.hEvent = event.value;
            DWORD count = 0;
            const auto started = write ? WriteFile(pipe, bytes, length, &count, &overlap)
                                       : ReadFile(pipe, bytes, length, &count, &overlap);
            if (!started) {
                if (GetLastError() != ERROR_IO_PENDING)
                    return false;
                if (WaitForSingleObject(event.value, 5000) != WAIT_OBJECT_0) {
                    CancelIoEx(pipe, &overlap);
                    GetOverlappedResult(pipe, &overlap, &count, TRUE);
                    return false;
                }
                if (!GetOverlappedResult(pipe, &overlap, &count, FALSE))
                    return false;
            }
            if (!count || count > length)
                return false;
            bytes += count;
            length -= count;
        }
        return true;
    }

    bool Client(HANDLE pipe)
    {
        ULONG pid = 0;
        if (!GetNamedPipeClientProcessId(pipe, &pid))
            return false;
        Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
        if (!process.value)
            return false;
        std::wstring path(32768, L'\0');
        DWORD size = static_cast<DWORD>(path.size());
        if (!QueryFullProcessImageNameW(process.value, 0, path.data(), &size))
            return false;
        std::error_code error;
        return fs::equivalent(fs::path(path.substr(0, size)), settingsExecutable, error);
    }

    void Listen(HANDLE firstPipe, std::wstring name, std::wstring security) noexcept
    {
        HANDLE next = firstPipe;
        for (;;) {
            Handle pipe(next);
            Handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
            if (!pipe.value || !event.value)
                return;
            OVERLAPPED overlap{};
            overlap.hEvent = event.value;
            const bool connected = ConnectNamedPipe(pipe.value, &overlap) != FALSE;
            auto error = connected ? ERROR_SUCCESS : GetLastError();
            if (error == ERROR_IO_PENDING) {
                HANDLE waits[]{event.value, stop.value};
                auto wait = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
                DWORD ignored;
                if (wait != WAIT_OBJECT_0) {
                    CancelIoEx(pipe.value, &overlap);
                    GetOverlappedResult(pipe.value, &overlap, &ignored, TRUE);
                    return;
                }
                error = GetOverlappedResult(pipe.value, &overlap, &ignored, FALSE) ? ERROR_SUCCESS : GetLastError();
            }
            if ((error == ERROR_SUCCESS || error == ERROR_PIPE_CONNECTED) && Client(pipe.value)) {
                std::array<unsigned char, 16> packet{};
                if (Transfer(pipe.value, packet.data(), static_cast<DWORD>(packet.size()), false)) {
                    uint32_t magic;
                    uint64_t ticket;
                    int32_t action;
                    std::memcpy(&magic, packet.data(), 4);
                    std::memcpy(&ticket, packet.data() + 4, 8);
                    std::memcpy(&action, packet.data() + 12, 4);
                    unsigned char result = 0;
                    {
                        const std::lock_guard lock(mutex);
                        for (const auto& [key, record] : records)
                            if (record.ticket == ticket && magic == 0x31304e53 && action >= -1 &&
                                (action < 0 || static_cast<size_t>(action) < record.actions) && events.size() < 2048 &&
                                record.receiver && WaitForSingleObject(record.receiver->value, 0) == WAIT_TIMEOUT) {
                                events.push_back(static_cast<int64_t>(ticket));
                                events.push_back(action);
                                result = 0xac;
                                break;
                            }
                    }
                    if (Transfer(pipe.value, &result, 1, true)) {
                        unsigned char acknowledged;
                        Transfer(pipe.value, &acknowledged, 1, false);
                    }
                }
            }
            CloseHandle(pipe.value);
            pipe.value = nullptr;
            if (WaitForSingleObject(stop.value, 0) != WAIT_TIMEOUT)
                return;
            PSECURITY_DESCRIPTOR descriptor = nullptr;
            if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(security.c_str(), SDDL_REVISION_1, &descriptor,
                                                                      nullptr))
                return;
            SECURITY_ATTRIBUTES attributes{sizeof(attributes), descriptor, FALSE};
            next = CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
                                    PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_REJECT_REMOTE_CLIENTS, 1, 64, 64, 0,
                                    &attributes);
            LocalFree(descriptor);
            // The previous connection must be closed before accepting its replacement.
            if (next == INVALID_HANDLE_VALUE)
                return;
        }
    }

    void StartListener()
    {
        GUID id{};
        winrt::check_hresult(CoCreateGuid(&id));
        wchar_t text[40]{};
        Check(StringFromGUID2(id, text, 40) > 0, "could not identify notification activation session");
        pipeName = L"Stone.Notification." + std::to_wstring(GetCurrentProcessId()) + L"." + std::wstring(text + 1, 36);
        HANDLE token = nullptr;
        winrt::check_bool(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token));
        Handle owner(token);
        DWORD size = 0;
        GetTokenInformation(token, TokenUser, nullptr, 0, &size);
        std::vector<unsigned char> user(size);
        winrt::check_bool(GetTokenInformation(token, TokenUser, user.data(), size, &size));
        PWSTR sid = nullptr;
        winrt::check_bool(ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(user.data())->User.Sid, &sid));
        const auto security = L"D:P(A;;GA;;;" + std::wstring(sid) + L")";
        LocalFree(sid);
        PSECURITY_DESCRIPTOR descriptor = nullptr;
        winrt::check_bool(ConvertStringSecurityDescriptorToSecurityDescriptorW(security.c_str(), SDDL_REVISION_1,
                                                                               &descriptor, nullptr));
        SECURITY_ATTRIBUTES attributes{sizeof(attributes), descriptor, FALSE};
        const auto name = L"\\\\.\\pipe\\" + pipeName;
        const auto pipe = CreateNamedPipeW(
            name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_REJECT_REMOTE_CLIENTS, 1, 64, 64, 0, &attributes);
        LocalFree(descriptor);
        Check(pipe != INVALID_HANDLE_VALUE, "could not create notification activation pipe");
        std::thread([self = shared_from_this(), pipe, name, security] {
            self->Listen(pipe, name, security);
        }).detach();
    }

    std::wstring Activation(uint64_t ticket, int action)
    {
        const auto prefix = L"Stone.Notification." + std::to_wstring(GetCurrentProcessId()) + L".";
        return protocol + L"://" + std::to_wstring(GetCurrentProcessId()) + L"/" + pipeName.substr(prefix.size()) +
               L"/" + std::to_wstring(ticket) + L"/" + std::to_wstring(action);
    }

    bool Initialize(const fs::path& settings, const fs::path& androidData, const std::wstring& packageName,
                    const fs::path& diagnostic, DWORD receiver)
    {
        Apartment();
        const std::lock_guard lock(mutex);
        if (notifier)
            return true;
        settingsExecutable = fs::canonical(settings);
        diagnosticExecutable = diagnostic;
        recipient.value = OpenProcess(SYNCHRONIZE, FALSE, receiver ? receiver : GetCurrentProcessId());
        Check(recipient.value != nullptr, "Cannot track notification recipient");

        settingsExecutable = fs::canonical(settingsExecutable);
        const auto identity = MakeNotificationIdentity(settingsExecutable, diagnosticExecutable);
        appId = identity.appId;
        protocol = identity.protocol;
        group = Hash(fs::canonical(androidData).wstring() + L"\n" + packageName).substr(0, 32);
        auto candidate = ToastNotificationManager::CreateToastNotifier(appId);
        StartListener();
        notifier = candidate;
        return true;
    }

    bool ArtNotificationsAllowed()
    {
        Apartment();
        if (!notifier)
            return false;
        NotificationSetting setting;
        try {
            setting = notifier.Setting();
        }
        catch (const winrt::hresult_error& error) {
            if (error.code() != HRESULT_FROM_WIN32(ERROR_NOT_FOUND))
                throw;
            // The Windows notification database creates identity-less sender state
            // on its first Show, before Setting can be queried (also used by the
            // Windows Community Toolkit's PreRegisterIdentityLessApp).
            winrt::Windows::Data::Xml::Dom::XmlDocument document;
            document.LoadXml(
                L"<toast activationType=\"protocol\" launch=\"" + Activation(0, -1) +
                L"\"><visual><binding template=\"ToastGeneric\"><text>Stone</text></binding></visual><audio silent=\"true\"/></toast>");
            ToastNotification bootstrap(document);
            bootstrap.Tag(L"stone-bootstrap");
            bootstrap.Group(group + L".init");
            bootstrap.SuppressPopup(true);
            bootstrap.ExpirationTime(winrt::clock::now() + std::chrono::seconds(15));
            notifier.Show(bootstrap);
            ToastNotificationManager::History().Remove(L"stone-bootstrap", group + L".init", appId);
            setting = notifier.Setting();
        }
        int& previous = previousSetting;
        if (previous != static_cast<int>(setting)) {
            previous = static_cast<int>(setting);
            std::fprintf(stderr, "[notifications] Windows notification setting=%d\n", previous);
        }
        return setting == NotificationSetting::Enabled;
    }

    uint64_t PostArtNotification(const WindowsNotification& request)
    {
        Apartment();
        const std::lock_guard lock(mutex);
        Check(notifier != nullptr, "notification backend is not initialized");
        Check(records.size() < 256 || records.contains(request.key), "too many active notifications");
        const auto ticket = ++nextTicket;
        auto activation = [&](int action) {
            return request.route.empty() ? Activation(ticket, action)
                                         : protocol + L"://service/" + request.route + L"/" + std::to_wstring(action);
        };
        std::wstring xml = L"<toast activationType=\"protocol\" launch=\"" + Xml(activation(-1)) +
                           L"\"><visual><binding template=\"ToastGeneric\"><text>" + Xml(request.title) +
                           L"</text><text>" + Xml(request.text) + L"</text>";
        if (request.indeterminate || request.maximum > 0) {
            const auto value =
                request.indeterminate
                    ? L"indeterminate"
                    : std::to_wstring(std::clamp(static_cast<double>(request.progress) / request.maximum, 0.0, 1.0));
            xml += L"<progress title=\"\" value=\"" + value + L"\" valueStringOverride=\"\" status=\" \"/>";
        }
        xml += L"</binding></visual>";
        if (!request.actions.empty()) {
            xml += L"<actions>";
            for (size_t index = 0; index < request.actions.size(); ++index)
                xml += L"<action content=\"" + Xml(request.actions[index]) +
                       L"\" activationType=\"protocol\" arguments=\"" + Xml(activation(static_cast<int>(index))) +
                       L"\"/>";
            xml += L"</actions>";
        }
        if (request.silent)
            xml += L"<audio silent=\"true\"/>";
        xml += L"</toast>";
        winrt::Windows::Data::Xml::Dom::XmlDocument document;
        document.LoadXml(xml);
        ToastNotification toast(document);
        toast.Tag(Hash(request.key).substr(0, 32));
        toast.Group(group);
        toast.SuppressPopup(request.suppressPopup);
        toast.Dismissed(
            [weak = weak_from_this(), ticket, route = request.route](const auto&, const ToastDismissedEventArgs& args) {
                if (args.Reason() != ToastDismissalReason::UserCanceled)
                    return;
                if (auto self = weak.lock()) {
                    const std::lock_guard lock(self->mutex);
                    if (!route.empty()) {
                        if (self->routeEvents.size() < 1024)
                            self->routeEvents.push_back(route);
                    }
                    else if (self->events.size() < 2048) {
                        self->events.push_back(static_cast<int64_t>(ticket));
                        self->events.push_back(-2);
                    }
                }
            });
        auto receiver =
            std::make_shared<Handle>(request.recipient ? OpenProcess(SYNCHRONIZE, FALSE, request.recipient) : nullptr);
        if (!request.recipient)
            Check(DuplicateHandle(GetCurrentProcess(), recipient.value, GetCurrentProcess(), &receiver->value, 0, FALSE,
                                  DUPLICATE_SAME_ACCESS),
                  "Cannot retain notification receiver");
        Check(receiver->value != nullptr, "Cannot track notification receiver");
        notifier.Show(toast);
        records.insert_or_assign(request.key, Record{ticket, request.actions.size(), toast, receiver, request.route});
        return ticket;
    }

    void RemoveArtNotification(const std::wstring& key)
    {
        Apartment();
        const std::lock_guard lock(mutex);
        if (!notifier)
            return;
        ToastNotificationManager::History().Remove(Hash(key).substr(0, 32), group, appId);
        records.erase(key);
    }

    void RemoveArtNotifications()
    {
        Apartment();
        const std::lock_guard lock(mutex);
        if (!notifier)
            return;
        ToastNotificationManager::History().RemoveGroup(group, appId);
        records.clear();
        ToastNotificationManager::History().RemoveGroup(group + L".init", appId);
    }

    std::vector<int64_t> PollArtNotifications()
    {
        const std::lock_guard lock(mutex);
        const auto count = std::min<size_t>(64, events.size());
        std::vector<int64_t> result(events.begin(), events.begin() + count);
        events.erase(events.begin(), events.begin() + count);
        return result;
    }

    std::vector<std::wstring> ReadArtNotificationHistory()
    {
        Apartment();
        const std::lock_guard lock(mutex);
        std::vector<std::wstring> result;
        if (notifier)
            for (auto toast : ToastNotificationManager::History().GetHistory(appId))
                if (toast.Group() == group)
                    result.emplace_back(toast.Content().GetXml());
        return result;
    }

    std::vector<std::wstring> PollRoutes()
    {
        const std::lock_guard lock(mutex);
        std::vector<std::wstring> result(routeEvents.begin(), routeEvents.end());
        routeEvents.clear();
        return result;
    }

    void SynchronizeRoutes(const std::vector<std::wstring>& routes)
    {
        Apartment();
        const std::lock_guard lock(mutex);
        for (auto it = records.begin(); it != records.end();) {
            const auto& record = it->second;
            const bool remove = record.route.empty()
                                    ? !record.receiver || WaitForSingleObject(record.receiver->value, 0) != WAIT_TIMEOUT
                                    : std::find(routes.begin(), routes.end(), record.route) == routes.end();
            if (remove) {
                ToastNotificationManager::History().Remove(Hash(it->first).substr(0, 32), group, appId);
                it = records.erase(it);
            }
            else
                ++it;
        }
    }
};

WindowsNotifications::WindowsNotifications(const fs::path& settings, const fs::path& androidData,
                                           const std::wstring& package, const fs::path& diagnostic, uint32_t recipient)
    : state_(std::make_shared<State>())
{
    state_->Check(state_->stop.value != nullptr, "Cannot create notification host stop event");
    state_->Initialize(settings, androidData, package, diagnostic, recipient);
}

WindowsNotifications::~WindowsNotifications()
{
    SetEvent(state_->stop.value);
}

bool WindowsNotifications::Allowed()
{
    return state_->ArtNotificationsAllowed();
}

uint64_t WindowsNotifications::Post(const WindowsNotification& notification)
{
    return state_->PostArtNotification(notification);
}

void WindowsNotifications::Remove(const std::wstring& key)
{
    state_->RemoveArtNotification(key);
}

void WindowsNotifications::RemoveAll()
{
    state_->RemoveArtNotifications();
}

std::vector<int64_t> WindowsNotifications::Poll()
{
    return state_->PollArtNotifications();
}

std::vector<std::wstring> WindowsNotifications::PollRoutes()
{
    return state_->PollRoutes();
}

void WindowsNotifications::SynchronizeRoutes(const std::vector<std::wstring>& routes)
{
    state_->SynchronizeRoutes(routes);
}

std::vector<std::wstring> WindowsNotifications::History()
{
    return state_->ReadArtNotificationHistory();
}
}

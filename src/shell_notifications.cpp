#include "shell_notifications.hpp"
#include "notification_text.hpp"

#include <windows.h>

#include <knownfolders.h>
#include <propidl.h>
#include <propkey.h>
#include <propvarutil.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include <atomic>
#include <fstream>
#include <mutex>
#include <string>

#include <winrt/Windows.Data.Xml.Dom.h>
#include <winrt/Windows.UI.Notifications.h>
#include <winrt/base.h>

#pragma comment(lib, "propsys.lib")

namespace scrap::hot_reload::shell_notifications {
namespace {

    using notification_text::canonical_path;
    using notification_text::narrow;
    using notification_text::vscode_uri;
    using notification_text::xml_escape;

    using Microsoft::WRL::ComPtr;

    HANDLE g_thread{};
    DWORD g_thread_id{};
    HANDLE g_ready_event{};
    HWND g_window{};
    HICON g_icon{};
    bool g_owns_icon{};
    bool g_toast_ready{};
    std::mutex g_mutex;
    std::wstring g_path;
    std::wstring g_game_path;

    constexpr UINT kNotifyMessage = WM_APP + 1;
    constexpr UINT kOpenMessage = WM_APP + 2;
    constexpr UINT kStopMessage = WM_APP + 3;
    constexpr UINT kNotificationId = 1;
    constexpr wchar_t kAppUserModelId[] = L"ScrapMechanic.LuaHotReload";
    constexpr wchar_t kShortcutName[] = L"Scrap Mechanic Lua Hot Reload.lnk";

    // -----------------------------------------------------------------------------
    // Shared notification state and diagnostics
    // -----------------------------------------------------------------------------

    void diagnostic_log(const std::string &message) {
        OutputDebugStringA((std::string("[HOTRELOAD-NOTIFY] ") + message + "\n").c_str());
        wchar_t temp_path[MAX_PATH]{};
        const auto length = GetTempPathW(static_cast<DWORD>(std::size(temp_path)), temp_path);
        if (!length || length >= std::size(temp_path))
            return;
        std::ofstream log(std::wstring(temp_path) + L"scrap_lua_hot_reload.log", std::ios::app);
        if (log)
            log << "[NOTIFY] " << message << '\n';
    }

    bool get_game_path() {
        wchar_t path[32768]{};
        const auto length = GetModuleFileNameW(nullptr, path, static_cast<DWORD>(std::size(path)));
        if (!length || length >= std::size(path)) {
            diagnostic_log("GetModuleFileNameW failed for game executable");
            return false;
        }
        g_game_path = canonical_path(std::wstring(path, length));
        diagnostic_log("game executable=" + narrow(g_game_path));
        return true;
    }

    // -----------------------------------------------------------------------------
    // Windows identity and icon discovery
    // -----------------------------------------------------------------------------

    HWND find_game_window() {
        HWND result{};
        EnumWindows(
            [](HWND window, LPARAM parameter) {
                DWORD process_id{};
                GetWindowThreadProcessId(window, &process_id);
                if (process_id == GetCurrentProcessId() && IsWindowVisible(window)) {
                    *reinterpret_cast<HWND *>(parameter) = window;
                    return FALSE;
                }
                return TRUE;
            },
            reinterpret_cast<LPARAM>(&result));
        return result;
    }

    HICON game_window_icon() {
        const auto game_window = find_game_window();
        if (!game_window)
            return nullptr;
        for (const auto kind : {ICON_SMALL2, ICON_SMALL, ICON_BIG}) {
            if (const auto icon = reinterpret_cast<HICON>(SendMessageW(game_window, WM_GETICON, kind, 0)))
                return icon;
        }
        if (const auto icon = reinterpret_cast<HICON>(GetClassLongPtrW(game_window, GCLP_HICONSM)))
            return icon;
        return reinterpret_cast<HICON>(GetClassLongPtrW(game_window, GCLP_HICON));
    }

    // -----------------------------------------------------------------------------
    // Toast registration and delivery
    // -----------------------------------------------------------------------------

    bool register_app_user_model_shortcut() {
        if (g_game_path.empty())
            return false;

        PWSTR programs_path{};
        const auto folder_result = SHGetKnownFolderPath(FOLDERID_Programs, KF_FLAG_DEFAULT, nullptr, &programs_path);
        if (FAILED(folder_result)) {
            diagnostic_log("SHGetKnownFolderPath(FOLDERID_Programs) failed hr=" + std::to_string(folder_result));
            return false;
        }
        const std::wstring shortcut_path = std::wstring(programs_path) + L"\\" + kShortcutName;
        CoTaskMemFree(programs_path);

        ComPtr<IShellLinkW> link;
        auto result = CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link));
        if (FAILED(result)) {
            diagnostic_log("CoCreateInstance(CLSID_ShellLink) failed hr=" + std::to_string(result));
            return false;
        }
        result = link->SetPath(g_game_path.c_str());
        if (FAILED(result)) {
            diagnostic_log("IShellLink::SetPath failed hr=" + std::to_string(result));
            return false;
        }
        result = link->SetIconLocation(g_game_path.c_str(), 0);
        if (FAILED(result)) {
            diagnostic_log("IShellLink::SetIconLocation failed hr=" + std::to_string(result));
            return false;
        }

        ComPtr<IPropertyStore> property_store;
        result = link.As(&property_store);
        if (FAILED(result)) {
            diagnostic_log("IShellLink::QueryInterface(IPropertyStore) failed hr=" + std::to_string(result));
            return false;
        }
        PROPVARIANT value{};
        result = InitPropVariantFromString(kAppUserModelId, &value);
        if (SUCCEEDED(result)) {
            result = property_store->SetValue(PKEY_AppUserModel_ID, value);
            PropVariantClear(&value);
        }
        if (FAILED(result)) {
            diagnostic_log("IPropertyStore::SetValue(PKEY_AppUserModel_ID) failed hr=" + std::to_string(result));
            return false;
        }
        result = property_store->Commit();
        if (FAILED(result)) {
            diagnostic_log("IPropertyStore::Commit failed hr=" + std::to_string(result));
            return false;
        }

        ComPtr<IPersistFile> persist;
        result = link.As(&persist);
        if (FAILED(result)) {
            diagnostic_log("IShellLink::QueryInterface(IPersistFile) failed hr=" + std::to_string(result));
            return false;
        }
        result = persist->Save(shortcut_path.c_str(), TRUE);
        if (FAILED(result)) {
            diagnostic_log("IPersistFile::Save failed hr=" + std::to_string(result));
            return false;
        }
        diagnostic_log("registered AUMID=" + narrow(kAppUserModelId) + " shortcut=" + narrow(shortcut_path));
        return true;
    }

    // The URI is built outside the WinRT code so encoding rules remain testable
    // without creating a notification window or initializing an apartment.

    bool show_toast(const std::wstring &path) {
        if (!g_toast_ready || path.empty())
            return false;
        try {
            const auto action_uri = vscode_uri(path);
            if (action_uri.empty())
                return false;
            const std::wstring xml = L"<toast><visual><binding template=\"ToastGeneric\"><text>"
                                     L"File change detected</text><text>" +
                                     xml_escape(path) +
                                     L"</text></binding></visual><actions><action content=\"Open in VS Code\" "
                                     L"activationType=\"protocol\" arguments=\"" +
                                     xml_escape(action_uri) + L"\"/></actions></toast>";
            winrt::Windows::Data::Xml::Dom::XmlDocument document;
            document.LoadXml(xml);
            auto notifier = winrt::Windows::UI::Notifications::ToastNotificationManager::CreateToastNotifier(
                winrt::hstring(kAppUserModelId));
            notifier.Show(winrt::Windows::UI::Notifications::ToastNotification(document));
            diagnostic_log("toast delivered path=" + narrow(path));
            return true;
        } catch (const winrt::hresult_error &error) {
            diagnostic_log("toast delivery failed hr=" + std::to_string(error.code().value) +
                           " message=" + narrow(error.message().c_str()));
            return false;
        } catch (...) {
            diagnostic_log("toast delivery failed with unknown exception");
            return false;
        }
    }

    // -----------------------------------------------------------------------------
    // Legacy Shell notification fallback
    // -----------------------------------------------------------------------------

    void open_in_vscode(const std::wstring &path) {
        if (path.empty())
            return;
        const std::wstring arguments = L"--goto \"" + path + L"\"";
        ShellExecuteW(nullptr, L"open", L"code.cmd", arguments.c_str(), nullptr, SW_SHOWNORMAL);
    }

    void show_legacy_balloon(HWND window, const std::wstring &path) {
        diagnostic_log("activating legacy Shell notification fallback");
        NOTIFYICONDATAW notification{};
        notification.cbSize = sizeof(notification);
        notification.hWnd = window;
        notification.uID = kNotificationId;
        notification.uFlags = NIF_INFO | NIF_ICON;
        notification.hIcon = g_icon;
        notification.dwInfoFlags = NIIF_USER;
        notification.hBalloonIcon = g_icon;
        wcsncpy_s(notification.szInfoTitle, L"File change detected", _TRUNCATE);
        wcsncpy_s(notification.szInfo, path.c_str(), _TRUNCATE);
        Shell_NotifyIconW(NIM_MODIFY, &notification);
    }

    // -----------------------------------------------------------------------------
    // Notification window and worker lifecycle
    // -----------------------------------------------------------------------------

    LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM, LPARAM lparam) {
        if (message == kNotifyMessage) {
            diagnostic_log("notification window received reload message");
            std::wstring path;
            {
                std::lock_guard lock(g_mutex);
                path = g_path;
            }
            diagnostic_log(
                "notification dispatch path=" + narrow(path) + " toast_ready=" + std::to_string(g_toast_ready ? 1 : 0));
            if (!show_toast(path))
                show_legacy_balloon(window, path);
            return 0;
        }
        if (message == kOpenMessage && lparam == NIN_BALLOONUSERCLICK) {
            std::wstring path;
            {
                std::lock_guard lock(g_mutex);
                path = g_path;
            }
            open_in_vscode(path);
            return 0;
        }
        if (message == kStopMessage) {
            // The window owns the notification icon, so destruction must be
            // initiated on this thread rather than by the unloading thread.
            DestroyWindow(window);
            return 0;
        }
        if (message == WM_DESTROY) {
            NOTIFYICONDATAW notification{};
            notification.cbSize = sizeof(notification);
            notification.hWnd = window;
            notification.uID = kNotificationId;
            Shell_NotifyIconW(NIM_DELETE, &notification);
            PostQuitMessage(0);
            return 0;
        }
        return DefWindowProcW(window, message, 0, lparam);
    }

    DWORD WINAPI notification_loop(void *) {
        bool apartment_ready = false;
        try {
            winrt::init_apartment(winrt::apartment_type::multi_threaded);
            apartment_ready = true;
        } catch (const winrt::hresult_error &error) {
            diagnostic_log("WinRT apartment initialization failed hr=" + std::to_string(error.code().value) +
                           " message=" + narrow(error.message().c_str()));
        } catch (...) {
            diagnostic_log("WinRT apartment initialization failed with unknown exception");
        }
        get_game_path();
        if (apartment_ready && register_app_user_model_shortcut()) {
            try {
                auto notifier = winrt::Windows::UI::Notifications::ToastNotificationManager::CreateToastNotifier(
                    winrt::hstring(kAppUserModelId));
                (void)notifier;
                g_toast_ready = true;
                diagnostic_log("WinRT toast initialization succeeded");
            } catch (const winrt::hresult_error &error) {
                diagnostic_log("WinRT toast initialization failed hr=" + std::to_string(error.code().value) +
                               " message=" + narrow(error.message().c_str()));
            } catch (...) {
                diagnostic_log("WinRT toast initialization failed with unknown exception");
            }
        } else {
            diagnostic_log("WinRT toast unavailable; shortcut registration or COM initialization failed");
        }

        const auto instance = GetModuleHandleW(nullptr);
        WNDCLASSW window_class{};
        window_class.hInstance = instance;
        window_class.lpfnWndProc = window_proc;
        window_class.lpszClassName = L"ScrapMechanicLuaHotReloadNotification";
        RegisterClassW(&window_class);

        const auto window = CreateWindowExW(0,
            window_class.lpszClassName,
            L"",
            0,
            0,
            0,
            0,
            0,
            HWND_MESSAGE,
            nullptr,
            instance,
            nullptr);
        if (!window) {
            diagnostic_log("CreateWindowExW notification window failed");
            if (g_ready_event)
                SetEvent(g_ready_event);
            if (apartment_ready)
                winrt::uninit_apartment();
            return 0;
        }
        g_window = window;

        NOTIFYICONDATAW notification{};
        notification.cbSize = sizeof(notification);
        notification.hWnd = window;
        notification.uID = kNotificationId;
        notification.uFlags = NIF_MESSAGE | NIF_ICON;
        notification.uCallbackMessage = kOpenMessage;
        g_icon = game_window_icon();
        if (!g_icon && !g_game_path.empty()) {
            HICON large_icon{};
            HICON small_icon{};
            if (ExtractIconExW(g_game_path.c_str(), 0, &large_icon, &small_icon, 1)) {
                g_icon = small_icon ? small_icon : large_icon;
                g_owns_icon = true;
            }
            if (large_icon && large_icon != g_icon)
                DestroyIcon(large_icon);
        }
        if (!g_icon)
            g_icon = LoadIconW(nullptr, IDI_APPLICATION);
        notification.hIcon = g_icon;
        Shell_NotifyIconW(NIM_ADD, &notification);
        if (g_ready_event)
            SetEvent(g_ready_event);
        diagnostic_log("notification window ready");

        MSG message{};
        while (GetMessageW(&message, nullptr, 0, 0) > 0)
            DispatchMessageW(&message);
        if (g_icon && g_owns_icon)
            DestroyIcon(g_icon);
        g_icon = nullptr;
        g_owns_icon = false;
        g_toast_ready = false;
        g_window = nullptr;
        if (apartment_ready)
            winrt::uninit_apartment();
        g_thread_id = 0;
        return 0;
    }

} // namespace

bool start() noexcept {
    if (g_thread)
        return true;
    g_ready_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!g_ready_event) {
        diagnostic_log("notification readiness event creation failed");
        return false;
    }
    g_thread = CreateThread(nullptr, 0, notification_loop, nullptr, 0, &g_thread_id);
    if (!g_thread) {
        diagnostic_log("notification thread creation failed");
        CloseHandle(g_ready_event);
        g_ready_event = nullptr;
        return false;
    }
    if (WaitForSingleObject(g_ready_event, 5000) != WAIT_OBJECT_0)
        diagnostic_log("notification thread readiness timed out; fallback remains available");
    CloseHandle(g_ready_event);
    g_ready_event = nullptr;
    return g_thread != nullptr;
}

void stop() noexcept {
    HWND window{};
    {
        std::lock_guard lock(g_mutex);
        window = g_window;
    }
    if (window)
        PostMessageW(window, kStopMessage, 0, 0);
    else if (g_thread_id)
        PostThreadMessageW(g_thread_id, WM_QUIT, 0, 0);
    if (g_thread) {
        // Never unload the DLL while the worker may still execute code from it.
        // The worker is event-driven and cancellation is posted above, so an
        // unbounded join is safer than closing a live thread handle.
        WaitForSingleObject(g_thread, INFINITE);
        CloseHandle(g_thread);
        g_thread = nullptr;
    }
    g_thread_id = 0;
}

void notify_reloaded(const std::wstring &path) noexcept {
    if (path.empty()) {
        diagnostic_log("notify_reloaded ignored empty path");
        return;
    }
    if (!g_window) {
        diagnostic_log("notify_reloaded ignored because notification window is unavailable");
        return;
    }
    {
        std::lock_guard lock(g_mutex);
        g_path = path;
    }
    const auto posted = PostMessageW(g_window, kNotifyMessage, 0, 0);
    diagnostic_log("notify_reloaded posted=" + std::to_string(posted ? 1 : 0) +
                   " error=" + std::to_string(posted ? ERROR_SUCCESS : GetLastError()) + " path=" + narrow(path));
}

} // namespace scrap::hot_reload::shell_notifications

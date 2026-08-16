#include "source_service.hpp"

#include "source_paths.hpp"

#include "scrap_mechanic_sdk/api.hpp"

#include <array>
#include <atomic>
#include <cstdlib>
#include <string>
#include <string_view>

namespace scrap::hot_reload::source_service {
namespace {

    source_paths::RootSet g_paths;
    std::array<HANDLE, source_paths::RootSet::max_roots> g_threads{};
    std::array<HANDLE, source_paths::RootSet::max_roots> g_directories{};
    std::atomic_bool g_stop{};

    bool resolve_path(void *, const std::string *path, std::wstring &physical) noexcept {
        return path && g_paths.resolve(*path, physical);
    }

    bool logical_path(void *, const std::wstring &physical, std::string &logical) noexcept {
        return g_paths.logical(physical, logical);
    }

    bool read_source(void *, scrap::sdk::lua::SourceBuffer *buffer, const std::string *path) noexcept {
        if (!buffer)
            return false;
        buffer->data = nullptr;
        buffer->size = 0;
        buffer->owned = 1;

        std::wstring physical;
        if (!path || !g_paths.resolve(*path, physical))
            return false;
        const auto file = CreateFileW(physical.c_str(),
            GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr,
            OPEN_EXISTING,
            0,
            nullptr);
        if (file == INVALID_HANDLE_VALUE)
            return false;

        LARGE_INTEGER length{};
        const bool valid = GetFileSizeEx(file, &length) && length.QuadPart > 0 &&
                           length.QuadPart <= static_cast<LONGLONG>(UINT32_MAX);
        if (!valid) {
            CloseHandle(file);
            return false;
        }

        const auto size = static_cast<DWORD>(length.QuadPart);
        auto *data = std::malloc(size);
        DWORD read = 0;
        const bool complete = data && ReadFile(file, data, size, &read, nullptr) && read == size;
        CloseHandle(file);
        if (!complete) {
            std::free(data);
            return false;
        }

        buffer->data = data;
        buffer->size = size;
        return true;
    }

    DWORD WINAPI watch(void *parameter) {
        const auto index = static_cast<std::size_t>(reinterpret_cast<std::uintptr_t>(parameter));
        const auto directory_path = g_paths.root(index);
        const auto directory = CreateFileW(directory_path.c_str(),
            FILE_LIST_DIRECTORY,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr,
            OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,
            nullptr);
        g_directories[index] = directory;
        if (directory == INVALID_HANDLE_VALUE)
            return 0;

        const auto event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!event) {
            CloseHandle(directory);
            return 0;
        }

        std::array<std::byte, 64 * 1024> buffer{};
        while (!g_stop.load(std::memory_order_acquire)) {
            OVERLAPPED overlapped{};
            overlapped.hEvent = event;
            ResetEvent(event);
            DWORD bytes = 0;
            const auto requested = ReadDirectoryChangesW(directory,
                buffer.data(),
                static_cast<DWORD>(buffer.size()),
                TRUE,
                FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_SIZE |
                    FILE_NOTIFY_CHANGE_CREATION,
                &bytes,
                &overlapped,
                nullptr);
            if (!requested && GetLastError() != ERROR_IO_PENDING)
                break;
            if (WaitForSingleObject(event, INFINITE) != WAIT_OBJECT_0 || g_stop.load())
                break;
            if (!GetOverlappedResult(directory, &overlapped, &bytes, FALSE) || !bytes)
                continue;

            auto *entry = reinterpret_cast<const FILE_NOTIFY_INFORMATION *>(buffer.data());
            for (;;) {
                const std::wstring_view name(entry->FileName, entry->FileNameLength / sizeof(wchar_t));
                if (name.ends_with(L".lua") || name.ends_with(L".LUA")) {
                    scrap::sdk::lua::reload_bridge::notify_changed(g_paths.changed_file(index, std::wstring(name)));
                    break;
                }
                if (!entry->NextEntryOffset)
                    break;
                entry = reinterpret_cast<const FILE_NOTIFY_INFORMATION *>(
                    reinterpret_cast<const std::byte *>(entry) + entry->NextEntryOffset);
            }
        }

        CancelIoEx(directory, nullptr);
        CloseHandle(event);
        CloseHandle(directory);
        g_directories[index] = nullptr;
        return 0;
    }

} // namespace

bool start(HMODULE game_module) noexcept {
    g_stop.store(false, std::memory_order_release);
    g_paths.discover(game_module);
    scrap::sdk::lua::reload_bridge::configure({nullptr,
        [](void *, const std::string *path) noexcept { return path && source_paths::is_lua_source(*path); },
        read_source,
        resolve_path,
        logical_path});

    if (g_paths.size() == 0) {
        OutputDebugStringA("[HOTRELOAD] no Lua source roots discovered\n");
        return false;
    }

    bool all_started = true;
    for (std::size_t index = 0; index < g_paths.size(); ++index) {
        g_threads[index] = CreateThread(nullptr, 0, watch, reinterpret_cast<void *>(index), 0, nullptr);
        if (!g_threads[index]) {
            all_started = false;
            OutputDebugStringA("[HOTRELOAD] failed to create Lua watcher thread\n");
        }
    }
    if (!all_started) {
        stop();
        clear();
    }
    return all_started;
}

void stop() noexcept {
    g_stop.store(true, std::memory_order_release);
    for (std::size_t index = 0; index < g_paths.size(); ++index) {
        if (g_directories[index] && g_directories[index] != INVALID_HANDLE_VALUE)
            CancelIoEx(g_directories[index], nullptr);
    }
    for (std::size_t index = 0; index < g_paths.size(); ++index) {
        if (!g_threads[index])
            continue;
        WaitForSingleObject(g_threads[index], INFINITE);
        CloseHandle(g_threads[index]);
        g_threads[index] = nullptr;
    }
}

void clear() noexcept {
    g_paths.clear();
    scrap::sdk::lua::reload_bridge::clear();
}

} // namespace scrap::hot_reload::source_service

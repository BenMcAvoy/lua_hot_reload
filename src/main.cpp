#include <windows.h>
#include <tlhelp32.h>

#include <atomic>

#include <rivet/modding.h>

#include "scrap_mechanic_sdk/scrap_mechanic_sdk.hpp"
#include "shell_notifications.hpp"
#include "source_service.hpp"

namespace {

HMODULE g_module{};
std::atomic_bool g_stop{};
scrap::sdk::Interceptor<scrap::sdk::lua::NativeSourceReloaded>::Connection g_reload_connection;

void report_reload(const scrap::sdk::lua::NativeSourceReloaded &reload) {
    if (!reload.source_reload_succeeded) {
        scrap::sdk::game::console::write("Lua hot reload failed");
        return;
    }

    scrap::sdk::game::console::write("Lua hot reload complete");
    scrap::hot_reload::shell_notifications::notify_reloaded(reload.physical_path);
}

DWORD WINAPI bootstrap(void *) {
    HMODULE game{};
    for (unsigned attempt = 0; attempt != 120 && !g_stop.load(); ++attempt) {
        game = GetModuleHandleW(L"ScrapMechanic.exe");
        if (game)
            break;
        Sleep(250);
    }
    if (game) {
        (void)scrap::sdk::runtime::resolve_console(game);
    }
    const bool acquired = game && scrap::sdk::lifecycle::abi_version() == scrap::sdk::lifecycle::abi_version_value &&
                          scrap::sdk::lifecycle::acquire();
    const bool hooks_installed = acquired && scrap::sdk::lua::hooks::install(game, false);
    const bool watcher_started = hooks_installed && scrap::hot_reload::source_service::start(game);
    if (watcher_started) {
        g_reload_connection = scrap::sdk::lua::interceptors().source_reloaded.subscribe(report_reload);
        (void)scrap::hot_reload::shell_notifications::start();
    } else {
        if (hooks_installed)
            scrap::sdk::lua::hooks::remove();
        scrap::sdk::lifecycle::release();
        scrap::sdk::game::console::write("Lua hot reload could not start");
    }
    return 0;
}

// Rivet calls this once after loading the mod, outside DllMain and the loader
// lock. The worker keeps startup non-blocking while the native SDK resolves
// the game module and installs its hooks.
void start_mod() {
    g_stop.store(false, std::memory_order_release);
    if (auto thread = CreateThread(nullptr, 0, bootstrap, nullptr, 0, nullptr))
        CloseHandle(thread);
}

bool rivet_is_loaded() {
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, GetCurrentProcessId());
    if (snapshot == INVALID_HANDLE_VALUE)
        return false;

    MODULEENTRY32W module{};
    module.dwSize = sizeof(module);
    bool loaded = false;
    if (Module32FirstW(snapshot, &module)) {
        do {
            // The loader may be renamed on installation, so identify it by
            // one of its stable exported C ABI functions instead of a DLL name.
            if (GetProcAddress(module.hModule, "Rivet_EventRegisterType")) {
                loaded = true;
                break;
            }
        } while (Module32NextW(snapshot, &module));
    }
    CloseHandle(snapshot);
    return loaded;
}

DWORD WINAPI fallback_entrypoint(void *) {
    if (!rivet_is_loaded())
        start_mod();
    return 0;
}

} // namespace

extern "C" __declspec(dllexport) DWORD WINAPI LuaHotReload_Unload(HMODULE module) {
    g_stop.store(true, std::memory_order_release);
    scrap::hot_reload::shell_notifications::stop();
    scrap::hot_reload::source_service::stop();
    g_reload_connection.disconnect();
    scrap::sdk::lua::hooks::remove();
    scrap::hot_reload::source_service::clear();
    scrap::sdk::lifecycle::release();
    FreeLibraryAndExitThread(module ? module : g_module, 0);
    return 0;
}

extern "C" BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = module;
        DisableThreadLibraryCalls(module);

        // Do not run mod startup under the loader lock. Rivet invokes the
        // registered entrypoint itself when present; direct DLL injection has
        // no Rivet callback, so defer the same startup to a worker thread.
        if (auto thread = CreateThread(nullptr, 0, fallback_entrypoint, nullptr, 0, nullptr))
            CloseHandle(thread);
    } else if (reason == DLL_PROCESS_DETACH) {
        g_stop.store(true, std::memory_order_release);
    }
    return TRUE;
}

RIVET_REGISTER_MOD(start_mod)

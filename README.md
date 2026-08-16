# Lua hot reload

This is a Rivet-loaded mod built on the shared `scrap_mechanic_sdk.dll`.

Rivet owns discovery and loading. The Scrap Mechanic SDK remains a separate
shared native dependency used by this and other mods; it is not folded into
Rivet and is not compiled into this DLL.

After injection, the DLL installs event-driven Windows directory notifications
for the native script roots. A changed Lua file sets a pending notification;
the game thread then increments the real raw-cache generation field, allowing
the game's original update gate to invoke `LuaVM::reloadChangedScripts(LuaVM*)`.
That routine checks
the real source timestamps, reloads changed `.lua` cache entries, rebuilds
their environments, and executes dependent scripts in the engine's intended
order. This is the supported internal path we found; the loader callback and
`LuaManager_refreshVM` are intermediate implementation details and must not be
called directly.

During the native reload transaction, loose Lua paths are forced through the
source-reader route so stale raw-cache blobs cannot be executed. The DLL does
not poll files or invoke the reload routine from a background thread; only the
file notification and generation bump are asynchronous, while the game
performs the reload itself.

The implementation is split by responsibility:

- `source_paths` discovers game/mod roots and translates logical paths.
- `source_service` supplies loose Lua reads and owns directory watcher threads.
- `notification_text` contains pure path, XML, UTF-8, and VS Code URI helpers.
- `shell_notifications` owns the notification worker and dispatch policy;
  toast and legacy delivery remain separate conceptual paths inside it.
- `main.cpp` owns DLL lifecycle and connects the SDK hook to those services.

The runtime flow is:

```text
native detours
    ↓
SDK hook coordinator
    ↓
reload coordinator / interceptors
    ↓
hot-reload DLL policy
    ├── source watcher
    └── notifications
```

The SDK coordinator owns native hook state and deferred refresh state. The DLL
owns file watching, path policy, and user-facing notifications. No notification
worker calls native Lua code.

The current build profile is the same version-specific profile used by the SDK
and the LuaManager overlay. Rivet currently keeps loaded mods resident for the
process lifetime, so `LuaHotReload_Unload` is retained as an explicit cleanup
export for development tools and controlled shutdown, but Rivet does not call
it automatically.

Build output is staged as two Thunderstore-style packages under
`projects/rivet/Mods`: `BenMcAvoy-Lua_Hot_Reload-0.1.0` and
`BenMcAvoy-ScrapMechanicSDK-0.1.0`. Install both package directories in
Rivet's configured game `Mods` directory. The hot-reload package declares the
SDK package in its standard Thunderstore `dependencies` list.

Static evidence for the reload boundary is recorded in the active IDA
database: `LuaVM_reloadChangedScripts` at `0x140623D40`, which calls
`LuaVM_executeScript` after detecting changed cache entries. The prior
`LuaManager_refreshVM` approach at `0x1408309F0` was rejected by the engine
when invoked from the callback dispatcher.

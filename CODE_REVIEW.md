# Lua Hot Reload DLL Code Review

Status: living review, updated 2026-08-13

This is a living review. Add implementation notes, fixes, and verification results as the DLL evolves.

The canonical active target is now the Rivet mod `lua_hot_reload`.
Historical versioned targets were removed from the active xmake project; the
older entries below remain as historical test records only.

## Readability refactor — 2026-08-13

The active code now has explicit policy boundaries. Source-root discovery and
watching are separate, notification text conversion is isolated from Win32 and
WinRT delivery, and manager/VM refresh coordination is owned by the SDK's
`lua_reload_coordinator` module. Native detours remain synchronous; deferred
class refreshes are consumed only at the existing safe points.

The refactor intentionally preserves native addresses, resolver patterns,
cache behavior, notification behavior, and public APIs. Remaining native hook
installation and detour code is kept together temporarily because it shares the
same ABI function types and hook handles; it is documented with thread and
ownership comments before any further mechanical split.

The style cleanup also removed four unregistered detours and their unused ABI
aliases from the active SDK implementation. Keeping dormant hook bodies in the
native path made the file look more capable than it was and caused compiler
warnings; future hooks should be added only with an installation entry and a
runtime test.

## Functional baseline

The native reload path has been verified in Scrap Mechanic Survival:

- `LuaVM::reloadChangedScripts(vm)` reloads the changed source.
- The physical file path is converted to the game's logical path, such as `$SURVIVAL_DATA/Scripts/game/tools/Sledgehammer.lua`.
- The native `LuaManager::refreshVM(manager, logical_path)` call updates existing class instances.
- `client_onRefresh` executes on an existing instance.
- Newly added or changed class methods execute afterward.
- File-local variables are refreshed and visible to newly executed methods.
- `client_onUpdate` changes were observed executing continuously after reload.
- Two successive edits were tested successfully without a game crash.

The current release candidate is `lua_hot_reload.dll`, loaded by Rivet.
The functional baseline above was originally recorded against v21 and must be
repeated against v24 before publishing a production artifact.

The notification worker now has deterministic shutdown: stop requests are
posted to its owning window thread, and unload waits indefinitely for that
thread to exit before closing its handle. Watcher startup also fails closed if
any required directory worker cannot be created.

## 2026-08-12 rebinding fix pass

The earlier filesystem-only class-key reconstruction was not sufficient evidence
for an existing-instance refresh. The reload hook now captures the exact logical
Lua path supplied by the game's native script loader for the changed script and
passes that value to `LuaManager::refreshVM`; the filesystem mapping remains a
fallback when the loader does not expose a matching call.

IDA evidence for the current executable:

- `LuaManager_refreshVM` is `0x1408309F0`.
- It hashes its `std::string` argument, finds the native class entry, and rebuilds
  callback vectors including `client_onUpdate` at manager offsets `+0x2D8..+0x2E8`.
- `LuaManager_dispatchClientUpdateCallbacks` is `0x140830750` and dispatches the
  live callback records from that vector.

Runtime verification used PID `26180`, launched through Steam with
`-last_save -dev -console`, after the log reported `Loading screen time:
236.908s`. An already-instantiated Sledgehammer emitted `HOTRELOAD_REBIND_A`.
Two passive edits changed the same live instances to `HOTRELOAD_REBIND_B` and
then `HOTRELOAD_REBIND_C`. The DLL log recorded
`captured native Lua path=$SURVIVAL_DATA/Scripts/game/tools/Sledgehammer.lua`
and `source=native-loader` for the refresh. The probe was then removed; the
following reload produced no probe output and normal `20 3`/`client_onRefresh`
logs. The current game log showed no Lua traceback, access violation, or first-
chance exception loop during the test.

This verifies existing built-in Survival instances and repeated
`client_onUpdate` rebinding. Mod-instance mapping, manager/VM synchronization,
offset resolution, and deterministic worker shutdown remain separate review
items and are not claimed as fixed by this pass.

## Findings

### High priority

#### 1. Mod class instances may not receive native instance refresh

`logical_script_path()` only maps the first three built-in script roots. Mod roots are appended starting at index 3, so a mod file can be source-reloaded but skip the native `refreshVM` call.

Location: `projects/scrap_mechanic_sdk/src/lua_hooks.cpp:104`

Impact: changed methods in an existing mod instance may remain stale even though the source loader reads the new file.

Recommended fix: derive the logical path for mod scripts from the game's loaded logical path or add a validated mod-root-to-logical-root mapping based on the native loader's path records.

#### 2. DLL unload can race live watcher threads

`stop_native_script_notifications()` waits only two seconds and then closes each thread handle regardless of whether the thread exited. The DLL can subsequently unload while a watcher is still executing.

Location: `projects/scrap_mechanic_sdk/src/lua_hooks.cpp:612`

The notification service has the same pattern:

Location: `projects/lua_hot_reload/src/shell_notifications.cpp:401`

Impact: use-after-unload, access violations, or a stuck shutdown.

Recommended fix: make cancellation and thread join deterministic. Never unload while any worker handle is still running; use an explicit shutdown event and an unbounded or correctly bounded join with failure handling.

#### 3. Raw Lua cache bypass is globally active

`hooked_raw_cache_read()` reads loose Lua source for every recognized Lua path, even when no reload is active and even when the file was not changed.

Location: `projects/scrap_mechanic_sdk/src/lua_hooks.cpp:330`

Impact: all Lua cache reads are replaced by loose-source reads. This disables native caching globally and can affect packaged or cache-only scripts.

Recommended fix: limit the source bypass to the active reload and the changed logical file, unless global cache disabling is an explicit product requirement.

#### 4. Reload success is reported without validating refresh success

`hooked_reload_changed()` marks the reload successful whenever the native reload function returns. It does not validate compilation, the `refreshVM` result, or callback execution.

Location: `projects/scrap_mechanic_sdk/src/lua_hooks.cpp:475`

Impact: the console and toast can report completion after a failed or partial reload.

Recommended fix: propagate native return/error state, record refresh failures, and only increment the completed counter after the relevant instance refresh has succeeded.

#### 5. Reload state and manager/VM pairing are not fully synchronized

`g_reload_state`, `g_reload_manager`, `g_manager_vms`, and `g_manager_vm_count` are accessed from hooks, watcher/update paths, and notification/reporting paths without a shared synchronization strategy.

Locations:

- `projects/scrap_mechanic_sdk/src/lua_hooks.cpp:47`
- `projects/scrap_mechanic_sdk/src/lua_hooks.cpp:497`
- `projects/scrap_mechanic_sdk/src/lua_hooks.cpp:661`

Impact: concurrent VM reloads or multiple managers can produce stale pairing, lost paths, or corrupted diagnostic state.

Recommended fix: serialize reload requests, protect manager/VM pairing, and use a per-reload result object instead of shared mutable state.

### Medium priority

#### 6. Watcher events are not debounced or coalesced

The watcher stops processing a notification buffer after the first `.lua` entry.

Location: `projects/scrap_mechanic_sdk/src/lua_hooks.cpp:574`

Editors commonly emit temporary-file, rename, size, and last-write events. This can cause duplicate reloads or reload before the final file contents are stable.

Recommended fix: collect all relevant events, resolve the final existing `.lua` file, debounce briefly, and reload one batch per stable change set.

#### 7. Native offsets remain hardcoded

Examples include the cache manager global and VM/script-record offsets.

Locations:

- `projects/scrap_mechanic_sdk/src/lua_hooks.cpp:444`
- `projects/scrap_mechanic_sdk/src/lua_hooks.cpp:633`
- `projects/scrap_mechanic_sdk/src/lua_hooks.cpp:665`

Impact: binary updates can silently corrupt memory or disable reload.

Recommended fix: resolve globals and structure fields from validated patterns, surrounding references, or native manager state where possible. Keep version checks and fail closed.

#### 8. Notification globals have cross-thread races

`g_window`, `g_toast_ready`, and `g_thread` are read and written across threads without synchronization.

Location: `projects/lua_hot_reload/src/shell_notifications.cpp:30`

Recommended fix: use an explicit service state machine, atomic readiness/state flags, and a stable thread-owned window handle lifecycle.

#### 9. Unicode VS Code URIs may be malformed

`vscode_uri()` percent-encodes UTF-8 bytes and then constructs a `std::wstring` directly from those bytes.

Location: `projects/lua_hot_reload/src/shell_notifications.cpp:194`

Impact: non-ASCII paths may not open correctly in VS Code.

Recommended fix: convert the encoded UTF-8 URI to UTF-16 with `MultiByteToWideChar`.

### Low priority / cleanup

#### 10. Legacy reload code remains in the SDK

`prepare_reload()` appears unused and contains additional hardcoded VM/cache structure assumptions.

Location: `projects/scrap_mechanic_sdk/src/lua_hooks.cpp:423`

Recommended fix: remove it after confirming no external caller depends on it, or isolate it as a documented diagnostic experiment.

#### 11. Unused install parameter

`install_hooks(HMODULE game_module, bool install_client_update)` currently does not use `install_client_update`.

Location: `projects/scrap_mechanic_sdk/src/lua_hooks.cpp:753`

Recommended fix: remove the parameter or implement the intended conditional behavior.

#### 12. Many duplicate versioned xmake targets

`projects/lua_hot_reload/xmake.lua` contains a long series of nearly identical `v1` through `v21` targets.

Impact: maintenance overhead and increased chance of injecting an outdated artifact.

Recommended fix: keep one canonical target and use build metadata or an explicit output directory for experimental variants.

## Verification record

| Date | Build | Test | Result |
|---|---|---|---|
| 2026-08-12 | `lua_hot_reload_native_instance_refresh_v21` | Built with xmake | Passed |
| 2026-08-12 | `lua_hot_reload_native_instance_refresh_v21` | Existing Survival instance received changed `client_onRefresh` | Passed |
| 2026-08-12 | `lua_hot_reload_native_instance_refresh_v21` | New class method read changed file-local values | Passed |
| 2026-08-12 | `lua_hot_reload_native_instance_refresh_v21` | Changed `client_onUpdate` executed continuously | Passed |
| 2026-08-12 | `lua_hot_reload_native_instance_refresh_v21` | Current process remained responsive with no Lua traceback or exception loop | Passed |
| 2026-08-12 | `lua_hot_reload_native_instance_refresh_v21` | Existing Sledgehammer `client_onUpdate` changed A→B→C across two passive edits | Passed |
| 2026-08-12 | `lua_hot_reload_native_instance_refresh_v21` | Native-loader logical path captured and used for class refresh | Passed |

## Historical planned fix order

1. Make worker shutdown and DLL unload safe.
2. Add mod logical-path resolution and test existing mod instances.
3. Serialize reload state and manager/VM pairing.
4. Debounce and coalesce watcher events.
5. Restrict or explicitly document global raw-cache bypass behavior.
6. Report native compilation and instance-refresh failures accurately.
7. Resolve or validate hardcoded native offsets.
8. Remove dead code and duplicate build targets.

## Function-level details

The highest-impact issues are visible directly in the implementation:

### `logical_script_path(const std::wstring &physical)`

`lua_hooks.cpp:104-127` hard-codes three logical prefixes and loops only over root indexes `0..2`. `discover_native_script_roots()` appends mod roots at index `3` and above (`lua_hooks.cpp:195`). Therefore a mod file can be found and source-loaded by `resolve_loose_lua_path()`, but `logical_script_path()` returns no logical class name, so `hooked_reload_changed()` skips `LuaManager::refreshVM()` for that file.

### `hooked_raw_cache_read(...)`

`lua_hooks.cpp:330-346` calls `read_loose_lua()` for every recognized `.lua` cache read. Unlike `hooked_cache_membership()` and `hooked_lua_script_load()`, it does not check `g_reload_active` or compare the path with `last_changed_path()`. This is the function that globally disables the native raw cache.

### `hooked_reload_changed(LuaVM *vm)`

`lua_hooks.cpp:475-528` sets `g_reload_state.last_success = true` and increments `completed` immediately after the original reload returns (`:487-490`). The subsequent `refreshVM()` call is not checked for success, and no callback or class-refresh result is recorded. The function can therefore report success when only the Lua source table changed.

It also reads `g_reload_manager` and the `g_manager_vms` array while other hooks may update them (`:497-503`, `hooked_fixed_update()` at `:661-688`). There is no mutex or reload queue around that pairing.

### `native_script_watch_loop(void *parameter)`

`lua_hooks.cpp:531-600` scans the notification buffer but breaks at the first `.lua` entry (`:578-587`). It does not consume the rest of the buffer, debounce editor writes, or verify that the final file is stable before setting `g_native_change_pending`.

### `stop_native_script_notifications()` and `shell_notifications::stop()`

The SDK shutdown function (`lua_hooks.cpp:612-628`) and toast service shutdown (`shell_notifications.cpp:401-408`) wait two seconds, then close the thread handle regardless of whether `WaitForSingleObject()` returned `WAIT_OBJECT_0`. Closing the handle is not thread termination; a still-running thread can execute code after the DLL is unloaded.

### `prepare_reload(LuaManager *manager)`

`lua_hooks.cpp:423-473` performs a second reload-preparation strategy, including direct VM/list traversal and a hard-coded script-record timestamp field at `:462`. No call site was found in the current source; it is dead or experimental code unless an external consumer depends on it.

### `hooked_fixed_update(void *self)` and `hooked_vm_refresh(void *self, std::intptr_t argument)`

Both functions read the manager's VM from `self + 0x358` (`lua_hooks.cpp:665` and `:692`) and mutate the shared `g_manager_vms` array. This is a version-specific memory layout assumption plus an unsynchronized registry update.

### `bump_native_cache_generation(HMODULE game_module)`

The cache-manager global is now resolved from the unique RIP-relative reference in the IDA-confirmed `LuaVM_reloadChangedScripts` path. The resolver validates that the target lies inside the loaded image and disables generation bumping if resolution fails; the previous fixed RVA has been removed. The manager generation field remains accessed at `manager + 0x20`.

### `vscode_uri(const std::wstring &path)`

`shell_notifications.cpp:194-217` correctly percent-encodes UTF-8 bytes, but then constructs a wide string by widening each encoded byte (`:215`). That is only safe because the encoded URI is ASCII; the function should still explicitly convert the final ASCII/UTF-8 URI to UTF-16 and validate the conversion. More importantly, the current safe-character policy leaves `:` unescaped anywhere, which can produce ambiguous URI components for unusual paths.
## 2026-08-13 safe-point refresh fix

Implemented the callback-reentrancy fix from the functionality review.

- `LuaVM::reloadChangedScripts` now queues class refresh work instead of calling `LuaManager_refreshVM` inline.
- Manager/VM associations, pending requests, sequence numbers, and refresh-in-progress state are coordinated under one mutex.
- Refreshes drain after the resolved client/fixed callback dispatcher returns, and are rejected/deferred while the IDA-confirmed callback depth/guard is active.
- Duplicate VM/logical-path requests are coalesced; manager selection requires an exact VM match.

## 2026-08-13 SDK boundary cleanup

The active filesystem service now lives in `lua_hot_reload/src/source_service.cpp`. It owns game/mod root discovery, directory notifications, physical/logical path conversion, and loose Lua source reads. The SDK exposes only a source-provider callback boundary and native change submission; native Lua hooks, cache hooks, and manager/VM safe-point refresh remain SDK responsibilities.

The public SDK facade is split into `core.hpp`, `game.hpp`, and `lua.hpp`. New consumers should use `scrap::sdk::game`, `scrap::sdk::lua::hooks`, `scrap::sdk::lua::reload_bridge`, and `scrap::sdk::lua::diagnostics`; the older root-level facades remain compatibility shims.

The SDK no longer exposes hot-reload counters or `ReloadState`; completion is reported through the native `NativeSourceReloaded` event, while reload statistics and user-facing reporting live in the DLL. The unused SDK-side `prepare_reload` path and filesystem watcher implementation were removed.

High-priority review fixes completed: unload now stops watcher threads, disconnects events, removes native hooks, and only then clears the provider under a shared/exclusive callback lock; the manager singleton is resolved from the unique IDA-confirmed RIP-relative constructor reference instead of the old public RVA accessor. The resolver pattern was verified unique in the active IDB at `LuaManager_initializeRole`.
- The active manager-to-VM pairing uses the SDK overlay rather than the former direct `+0x358` read.
- Diagnostics distinguish queued, coalesced, deferred, skipped, begun, returned, and completed refreshes.

IDA evidence used: `LuaManager_dispatchClientUpdateCallbacks` at `0x140830750`, `LuaManager_refreshVM` at `0x1408309F0`, and the active callback state at manager offsets `0x48`/`0x59` confirmed by the current IDB.

Build artifact:

`projects/lua_hot_reload/build/windows/x64/release_native_refresh_v22/lua_hot_reload_native_instance_refresh_v22.dll`

Runtime verification on 2026-08-13:

- Fresh Steam-launched `-last_save -dev -console` game, PID 46776.
- World load completed in 239 seconds.
- Sledgehammer `client_onUpdate` was edited with a temporary `[SAFEPOINT_TEST] update_v22` print and then restored.
- The new DLL logged a queued request followed by one serialized `refresh begin`, `refresh returned`, and `refresh completed` after the dispatcher boundary.
- No new `ongoing callback`, sandbox violation, Lua panic, missing-animation error, crash, or first-chance exception loop was observed.
- Auxiliary VMs without an exact manager association were queued with a null manager and did not execute a guessed refresh.

Remaining limitation: unmatched auxiliary VM requests are safely deferred and need lifecycle/manager association cleanup or a later exact pairing before their class instances can be refreshed.

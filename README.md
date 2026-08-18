# Lua Hot Reload

Lua Hot Reload is a Rivet mod for Scrap Mechanic. It watches the game's Lua
source directories and asks the game to reload changed files through its own
native reload path.

It supports built-in Survival scripts and Lua files from installed mods when
those files are located in directories discovered by the game.

## Requirements

- Scrap Mechanic
- [Rivet](https://github.com/ReDoIngMods/Rivet)
- [Scrap Mechanic SDK](https://github.com/BenMcAvoy/scrap_mechanic_sdk)
- A Windows installation of the game

The SDK is declared as a Thunderstore dependency, so a Rivet package manager
can install it automatically.

## Installation

The recommended installation method is [r2modman for Scrap
Mechanic](https://thunderstore.io/c/scrap-mechanic/p/ebkr/r2modman/). Create or
select a Scrap Mechanic profile, then install Lua Hot Reload from the
Thunderstore package list. r2modman installs Rivet, the SDK, and the required
dependencies into the profile for you.

You do not need to copy files into the Scrap Mechanic installation directory.
For users who need to load the DLL outside r2modman, see [Manual
loading](#manual-loading) below.

The packages are kept together in the profile managed by r2modman:

```text
Mods/
├── BenMcAvoy-ScrapMechanicSDK-0.1.3/
│   └── scrap_mechanic_sdk.dll
└── BenMcAvoy-Lua_Hot_Reload-0.1.4/
    └── lua_hot_reload.dll
```

Start Scrap Mechanic normally through Rivet. The mod starts after Rivet loads
the DLL and installs the native hooks. No separate launcher or command-line
flag is required for normal use.

## Manual loading

The hot-reload DLL can also be loaded directly with an injector or with
`LoadLibraryW`. The Scrap Mechanic SDK must be loadable first because
`lua_hot_reload.dll` links against `scrap_mechanic_sdk.dll` and cannot start on
its own.

For manual loading, keep both DLLs in a staging directory outside the Scrap
Mechanic installation directory before loading the hot-reload DLL:

```text
manual-load/
├── scrap_mechanic_sdk.dll
└── lua_hot_reload.dll
```

Use matching x64 release builds. If the injector uses an absolute DLL path,
Windows will normally search the directory containing that DLL for its native
dependency. An injector that changes the DLL search path should add the
directory containing both files before calling `LoadLibraryW`. Code that uses
`LoadLibraryExW` should use `LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR` with the full
path to `lua_hot_reload.dll`.

Load `scrap_mechanic_sdk.dll` first when the injector controls the order, then
load `lua_hot_reload.dll`. Loading the hot-reload DLL directly also starts its
fallback bootstrap from `DllMain`, outside the loader lock. If Rivet is already
running, the fallback detects Rivet and leaves startup to Rivet's registered
mod entrypoint.

The manual loading order is therefore:

1. Start Scrap Mechanic and wait until its process is available.
2. Load `scrap_mechanic_sdk.dll` from the staging directory.
3. Load `lua_hot_reload.dll` from the staging directory.
4. Wait for the game world to finish loading before editing Lua files.

Do not unload either DLL by force while the game is running. The hot-reload DLL
exports `LuaHotReload_Unload` for injectors that support clean unloading. Call
that export first, wait for it to return, and only then unload the SDK if no
other mod is using it.

## How it works

The file watcher uses Windows directory change notifications. It does not poll
the filesystem and it does not execute Lua from a worker thread.

When a Lua file changes:

1. The watcher records the change.
2. The game thread receives a reload request.
3. The game's native cache generation is advanced.
4. The game's normal reload routine checks source timestamps and reloads the
   changed Lua source.
5. The SDK coordinates any class refresh at a safe game callback boundary.

The reload is performed by the game so its normal script ordering,
environments, and dependency handling remain in control.

## Notifications

After a successful reload, Windows displays a notification containing the
changed file path. The notification has an `Open in VS Code` button when VS
Code is installed. If Windows toast delivery is unavailable, the mod uses a
legacy shell notification instead.

## Building

The build expects the SDK and Rivet repositories next to this repository when
using the default paths:

```text
projects/
├── lua_hot_reload/
├── scrap_mechanic_sdk/
└── rivet/
```

Build the SDK first, then build the mod:

```powershell
cd projects/scrap_mechanic_sdk
git submodule update --init --recursive
xmake f -y -p windows -a x64 -m release
xmake b -r -y scrap_mechanic_sdk

cd ../lua_hot_reload
xmake f -y -p windows -a x64 -m release
xmake b -r -y lua_hot_reload
```

The GitHub Actions workflow checks out the SDK and Rivet headers, builds the
SDK dependency, builds the mod, validates the manifest and icon, and uploads
the Thunderstore zip as a workflow artifact.

## Source layout

- `source_paths` discovers script roots and maps physical files to game paths.
- `source_service` owns directory watchers and pending file changes.
- `notification_text` handles path, XML, UTF-8, and VS Code URI conversion.
- `shell_notifications` owns notification delivery and fallback behavior.
- `main.cpp` starts and stops the mod and connects it to the SDK.

## Limitations

This mod targets the current Scrap Mechanic executable. A game update can
change the native functions or data layouts used by the SDK. When the SDK
cannot validate a native feature, it disables that feature rather than using
an unverified address.

Rivet keeps loaded DLLs resident for the lifetime of the process. The mod
exports `LuaHotReload_Unload` for development tools and controlled shutdown,
but normal Rivet loading does not unload it automatically.

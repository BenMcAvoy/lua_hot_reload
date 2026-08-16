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

Install both Thunderstore packages through Rivet, or place both package
directories in Rivet's configured Mods directory:

```text
Mods/
├── BenMcAvoy-ScrapMechanicSDK-0.1.0/
│   └── scrap_mechanic_sdk.dll
└── BenMcAvoy-Lua_Hot_Reload-0.1.0/
    └── lua_hot_reload.dll
```

Start Scrap Mechanic normally through Rivet. The mod starts after Rivet loads
the DLL and installs the native hooks. No separate launcher or command-line
flag is required for normal use.

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

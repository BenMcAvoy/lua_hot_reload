set_project("lua_hot_reload")
set_version("0.1.5")
set_languages("c++20")
set_toolchains("clang-cl")

target("lua_hot_reload")
    set_kind("shared")
    local sdk_root = os.getenv("LUA_HOT_RELOAD_SDK_ROOT") or "../scrap_mechanic_sdk"
    local rivet_root = os.getenv("LUA_HOT_RELOAD_RIVET_ROOT") or "../rivet"
    local target_dir = os.getenv("LUA_HOT_RELOAD_TARGET_DIR")
        or (rivet_root .. "/Mods/BenMcAvoy-Lua_Hot_Reload-0.1.5")

    set_targetdir(target_dir)
    add_files("src/main.cpp", "src/shell_notifications.cpp", "src/notification_text.cpp", "src/source_service.cpp", "src/source_paths.cpp")
    add_linkdirs(sdk_root .. "/build/windows/x64/release")
    add_links("scrap_mechanic_sdk")
    add_includedirs(sdk_root .. "/include", rivet_root .. "/src/Lib/include")
    add_defines("WIN32_LEAN_AND_MEAN", "NOMINMAX", "UNICODE", "_UNICODE")
    add_cxxflags("/EHsc", "/W4", "/permissive-")
    add_ldflags("/SUBSYSTEM:WINDOWS")
    add_syslinks("kernel32", "user32", "shell32", "ole32", "propsys", "windowsapp")

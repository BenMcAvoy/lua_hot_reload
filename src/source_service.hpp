#pragma once

#include <windows.h>

#include <string>

namespace scrap::hot_reload::source_service {

bool start(HMODULE game_module) noexcept;
void stop() noexcept;
void clear() noexcept;

} // namespace scrap::hot_reload::source_service

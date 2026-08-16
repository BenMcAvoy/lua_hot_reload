#pragma once

#include <string>

namespace scrap::hot_reload::shell_notifications {

[[nodiscard]] bool start() noexcept;
void stop() noexcept;
void notify_reloaded(const std::wstring &path) noexcept;

} // namespace scrap::hot_reload::shell_notifications

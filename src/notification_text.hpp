#pragma once

#include <string>

namespace scrap::hot_reload::notification_text {

[[nodiscard]] std::wstring canonical_path(const std::wstring &path);
[[nodiscard]] std::string narrow(const std::wstring &value);
[[nodiscard]] std::wstring xml_escape(const std::wstring &value);
[[nodiscard]] std::wstring vscode_uri(const std::wstring &path);

} // namespace scrap::hot_reload::notification_text

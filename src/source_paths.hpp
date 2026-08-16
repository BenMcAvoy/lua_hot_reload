#pragma once

#include <windows.h>

#include <array>
#include <cstddef>
#include <shared_mutex>
#include <string>

namespace scrap::hot_reload::source_paths {

class RootSet {
  public:

    static constexpr std::size_t max_roots = 64;

    void discover(HMODULE game_module) noexcept;
    void clear() noexcept;

    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] std::wstring root(std::size_t index) const;
    [[nodiscard]] std::wstring changed_file(std::size_t index, const std::wstring &name) const;

    [[nodiscard]] bool resolve(const std::string &logical_path, std::wstring &physical_path) const noexcept;
    [[nodiscard]] bool logical(const std::wstring &physical_path, std::string &logical_path) const noexcept;

  private:

    std::array<std::wstring, max_roots> roots_{};
    std::size_t count_{};
    mutable std::shared_mutex mutex_;
};

[[nodiscard]] bool is_lua_source(const std::string &path) noexcept;
[[nodiscard]] std::wstring canonical(std::wstring path);

} // namespace scrap::hot_reload::source_paths

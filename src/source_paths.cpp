#include "source_paths.hpp"

#include <array>
#include <mutex>
#include <string_view>
#include <utility>

namespace scrap::hot_reload::source_paths {
namespace {

    bool is_file(const std::wstring &path) noexcept {
        const auto attributes = GetFileAttributesW(path.c_str());
        return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
    }

    std::wstring utf8_to_wide(const std::string &value) {
        const auto count = MultiByteToWideChar(CP_UTF8,
            MB_ERR_INVALID_CHARS,
            value.data(),
            static_cast<int>(value.size()),
            nullptr,
            0);
        if (!count)
            return {};

        std::wstring result(static_cast<std::size_t>(count), L'\0');
        return MultiByteToWideChar(CP_UTF8,
                   MB_ERR_INVALID_CHARS,
                   value.data(),
                   static_cast<int>(value.size()),
                   result.data(),
                   count)
                   ? result
                   : std::wstring{};
    }

    bool wide_to_utf8(const std::wstring &value, std::string &result) noexcept {
        const auto count = WideCharToMultiByte(CP_UTF8,
            WC_ERR_INVALID_CHARS,
            value.data(),
            static_cast<int>(value.size()),
            nullptr,
            0,
            nullptr,
            nullptr);
        if (!count)
            return false;

        result.resize(static_cast<std::size_t>(count));
        return WideCharToMultiByte(CP_UTF8,
                   WC_ERR_INVALID_CHARS,
                   value.data(),
                   static_cast<int>(value.size()),
                   result.data(),
                   count,
                   nullptr,
                   nullptr) != 0;
    }

    std::wstring candidate(const std::wstring &root, std::wstring relative) {
        while (relative.starts_with(L"\\") || relative.starts_with(L"/"))
            relative.erase(0, 1);

        for (;;) {
            const auto dot = relative.find(L".\\");
            if (dot == std::wstring::npos)
                break;
            relative.replace(dot, 2, L"\\");
        }

        const auto result = canonical(root + L"\\" + relative);
        return is_file(result) ? result : std::wstring{};
    }

} // namespace

std::wstring canonical(std::wstring path) {
    wchar_t full_path[32768]{};
    const auto length = GetFullPathNameW(path.c_str(), static_cast<DWORD>(std::size(full_path)), full_path, nullptr);
    return length && length < std::size(full_path) ? std::wstring(full_path, length) : std::move(path);
}

bool is_lua_source(const std::string &path) noexcept {
    if (path.size() < 4 || path.size() > 4096)
        return false;
    const std::string_view value(path);
    return value.ends_with(".lua") || value.ends_with(".LUA");
}

void RootSet::discover(HMODULE module) noexcept {
    std::unique_lock lock(mutex_);
    roots_ = {};
    count_ = 0;

    wchar_t module_path[32768]{};
    const auto length = GetModuleFileNameW(module, module_path, static_cast<DWORD>(std::size(module_path)));
    if (length && length < std::size(module_path)) {
        auto root = canonical(std::wstring(module_path, length));
        const auto separator = root.find_last_of(L"\\/");
        root.resize(separator == std::wstring::npos ? 0 : separator);

        for (unsigned level = 0; !root.empty() && level < 6; ++level) {
            if (GetFileAttributesW((root + L"\\Data\\Scripts").c_str()) != INVALID_FILE_ATTRIBUTES &&
                GetFileAttributesW((root + L"\\Survival\\Scripts").c_str()) != INVALID_FILE_ATTRIBUTES) {
                for (const auto *relative : {L"Survival\\Scripts", L"Data\\Scripts", L"ChallengeData\\Scripts"}) {
                    if (count_ == roots_.size())
                        break;
                    roots_[count_++] = root + L"\\" + relative;
                }
                break;
            }
            const auto parent = root.find_last_of(L"\\/");
            if (parent == std::wstring::npos)
                break;
            root.resize(parent);
        }
    }

    wchar_t app_data[MAX_PATH]{};
    const auto app_length = GetEnvironmentVariableW(L"APPDATA", app_data, static_cast<DWORD>(std::size(app_data)));
    if (!app_length || app_length >= std::size(app_data))
        return;

    const auto users = std::wstring(app_data) + L"\\Axolot Games\\Scrap Mechanic\\User\\*";
    WIN32_FIND_DATAW data{};
    const auto find = FindFirstFileW(users.c_str(), &data);
    if (find == INVALID_HANDLE_VALUE)
        return;

    do {
        if (!(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            continue;
        const std::wstring name(data.cFileName);
        if (!name.starts_with(L"User_"))
            continue;

        const auto mods = std::wstring(app_data) + L"\\Axolot Games\\Scrap Mechanic\\User\\" + name + L"\\Mods";
        if (GetFileAttributesW(mods.c_str()) != INVALID_FILE_ATTRIBUTES && count_ < roots_.size())
            roots_[count_++] = mods;
    } while (FindNextFileW(find, &data));

    FindClose(find);
}

void RootSet::clear() noexcept {
    std::unique_lock lock(mutex_);
    roots_ = {};
    count_ = 0;
}

std::size_t RootSet::size() const noexcept {
    std::shared_lock lock(mutex_);
    return count_;
}

std::wstring RootSet::root(std::size_t index) const {
    std::shared_lock lock(mutex_);
    return index < count_ ? roots_[index] : std::wstring{};
}

std::wstring RootSet::changed_file(std::size_t index, const std::wstring &name) const {
    const auto directory = root(index);
    return directory.empty() ? std::wstring{} : canonical(directory + L"\\" + name);
}

bool RootSet::resolve(const std::string &path, std::wstring &physical) const noexcept {
    std::shared_lock lock(mutex_);
    if (path.empty() || path.size() > 4096)
        return false;

    const auto logical = utf8_to_wide(path);
    if (logical.empty())
        return false;
    if ((logical.size() > 2 && logical[1] == L':') || logical.starts_with(L"\\\\")) {
        physical = canonical(logical);
        return is_file(physical);
    }

    const auto try_root = [&](std::wstring_view token, std::size_t index) {
        return logical.starts_with(token) && index < count_ ? candidate(roots_[index], logical.substr(token.size()))
                                                            : std::wstring{};
    };
    for (const auto [token, index] :
        std::array<std::pair<std::wstring_view, std::size_t>, 8>{{{L"$SURVIVAL_DATA/Scripts/", 0},
            {L"$SURVIVAL_DATA\\Scripts\\", 0},
            {L"$GAME_DATA/Scripts/", 1},
            {L"$GAME_DATA\\Scripts\\", 1},
            {L"$DATA/Scripts/", 1},
            {L"$DATA\\Scripts\\", 1},
            {L"$CHALLENGE_DATA/Scripts/", 2},
            {L"$CHALLENGE_DATA\\Scripts\\", 2}}}) {
        if (auto result = try_root(token, index); !result.empty()) {
            physical = std::move(result);
            return true;
        }
    }

    for (std::size_t index = 0; index < count_; ++index) {
        if (auto result = candidate(roots_[index], logical); !result.empty()) {
            physical = std::move(result);
            return true;
        }
    }
    return false;
}

bool RootSet::logical(const std::wstring &physical_path, std::string &logical_path) const noexcept {
    std::shared_lock lock(mutex_);
    const auto value = canonical(physical_path);
    constexpr std::array<std::string_view, 3> prefixes{"$SURVIVAL_DATA/Scripts/",
        "$GAME_DATA/Scripts/",
        "$CHALLENGE_DATA/Scripts/"};

    for (std::size_t index = 0; index < 3 && index < count_; ++index) {
        const auto root_path = canonical(roots_[index]);
        if (value.size() <= root_path.size() || _wcsnicmp(value.c_str(), root_path.c_str(), root_path.size()) != 0 ||
            value[root_path.size()] != L'\\')
            continue;

        auto relative = value.substr(root_path.size() + 1);
        for (auto &character : relative) {
            if (character == L'\\')
                character = L'/';
        }
        std::string utf8;
        if (!wide_to_utf8(relative, utf8))
            return false;
        logical_path.assign(prefixes[index]);
        logical_path += utf8;
        return true;
    }
    return false;
}

} // namespace scrap::hot_reload::source_paths

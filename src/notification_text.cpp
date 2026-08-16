#include "notification_text.hpp"

#include <windows.h>

#include <algorithm>

namespace scrap::hot_reload::notification_text {

std::wstring canonical_path(const std::wstring &path) {
    wchar_t full_path[32768]{};
    const auto length = GetFullPathNameW(path.c_str(), static_cast<DWORD>(std::size(full_path)), full_path, nullptr);
    if (length != 0 && length < std::size(full_path))
        return {full_path, length};
    return path;
}

std::string narrow(const std::wstring &value) {
    if (value.empty())
        return {};

    const int length = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (length <= 1)
        return {};

    std::string result(static_cast<std::size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, result.data(), length, nullptr, nullptr);
    result.pop_back();
    return result;
}

std::wstring xml_escape(const std::wstring &value) {
    std::wstring result;
    result.reserve(value.size());

    for (const wchar_t character : value) {
        switch (character) {
        case L'&':
            result += L"&amp;";
            break;
        case L'<':
            result += L"&lt;";
            break;
        case L'>':
            result += L"&gt;";
            break;
        case L'\"':
            result += L"&quot;";
            break;
        case L'\'':
            result += L"&apos;";
            break;
        default:
            result += character;
            break;
        }
    }

    return result;
}

std::wstring vscode_uri(const std::wstring &path) {
    const int length = WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (length <= 1)
        return {};

    std::string utf8(static_cast<std::size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1, utf8.data(), length, nullptr, nullptr);
    utf8.pop_back();
    std::replace(utf8.begin(), utf8.end(), '\\', '/');

    constexpr char hex[] = "0123456789ABCDEF";
    std::string encoded;
    encoded.reserve(utf8.size());

    for (const unsigned char character : utf8) {
        const bool safe = (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
                          (character >= '0' && character <= '9') || character == '-' || character == '_' ||
                          character == '.' || character == '~' || character == '/' || character == ':';

        if (safe) {
            encoded += static_cast<char>(character);
        } else {
            encoded += '%';
            encoded += hex[character >> 4];
            encoded += hex[character & 0x0f];
        }
    }

    return L"vscode://file/" + std::wstring(encoded.begin(), encoded.end());
}

} // namespace scrap::hot_reload::notification_text

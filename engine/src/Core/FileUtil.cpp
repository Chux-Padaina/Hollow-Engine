// Implementation of FileUtil.hpp.
#include "Hollow/Core/FileUtil.hpp"

#include <Windows.h>

#include <algorithm>
#include <fstream>
#include <iterator>
#include <stdexcept>

namespace Hollow {

std::string to_utf8(const std::filesystem::path& path) {
    const std::u8string text = path.u8string();
    return {reinterpret_cast<const char*>(text.data()), text.size()};
}

std::filesystem::path path_from_utf8(std::string_view utf8) {
    return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size()));
}

std::wstring utf8_to_wide(std::string_view utf8) {
    if (utf8.empty()) {
        return {};
    }
    const int length = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(std::max(length, 0)), L'\0');
    if (length > 0) {
        MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), length);
    }
    return out;
}

std::string wide_to_utf8(std::wstring_view wide) {
    if (wide.empty()) {
        return {};
    }
    const int length = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(std::max(length, 0)), '\0');
    if (length > 0) {
        WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), out.data(), length, nullptr, nullptr);
    }
    return out;
}

std::string read_text_file(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        throw std::runtime_error("could not open file: " + to_utf8(path));
    }
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

std::vector<std::uint8_t> read_binary_file(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        throw std::runtime_error("could not open file: " + to_utf8(path));
    }
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

void write_text_file(const std::filesystem::path& path, std::string_view text) {
    std::filesystem::path temp = path;
    temp += ".tmp";
    {
        std::ofstream file(temp, std::ios::binary | std::ios::trunc);
        if (!file) {
            throw std::runtime_error("could not write file: " + to_utf8(temp));
        }
        file.write(text.data(), static_cast<std::streamsize>(text.size()));
        if (!file) {
            throw std::runtime_error("write failed: " + to_utf8(temp));
        }
    }
    std::error_code ec;
    std::filesystem::rename(temp, path, ec);
    if (ec) {
        std::filesystem::remove(temp, ec);
        throw std::runtime_error("could not replace file: " + to_utf8(path));
    }
}

std::string make_relative_utf8(const std::filesystem::path& path, const std::filesystem::path& base) {
    if (path.empty()) {
        return {};
    }
    if (base.empty()) {
        return to_utf8(std::filesystem::path(path.lexically_normal().generic_u8string()));
    }
    std::error_code ec;
    const std::filesystem::path absolute = std::filesystem::absolute(path, ec).lexically_normal();
    const std::filesystem::path absolute_base = std::filesystem::absolute(base, ec).lexically_normal();
    // Only relativize on the same drive; a path that climbs more than a few
    // levels is clearer kept absolute.
    if (absolute.root_name() == absolute_base.root_name()) {
        const std::filesystem::path relative = absolute.lexically_relative(absolute_base);
        if (!relative.empty()) {
            int climbs = 0;
            for (const auto& part : relative) {
                if (part == "..") {
                    ++climbs;
                }
            }
            if (climbs <= 3) {
                return to_utf8(relative.generic_u8string());
            }
        }
    }
    return to_utf8(absolute.generic_u8string());
}

std::filesystem::path resolve_utf8(std::string_view stored, const std::filesystem::path& base) {
    std::filesystem::path path = path_from_utf8(stored);
    if (path.empty() || path.is_absolute() || base.empty()) {
        return path.lexically_normal();
    }
    return (base / path).lexically_normal();
}

std::filesystem::path executable_directory() {
    std::wstring buffer(MAX_PATH, L'\0');
    for (;;) {
        const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) {
            return std::filesystem::current_path();
        }
        if (length < buffer.size()) {
            buffer.resize(length);
            return std::filesystem::path(buffer).parent_path();
        }
        buffer.resize(buffer.size() * 2);
    }
}

} // namespace Hollow

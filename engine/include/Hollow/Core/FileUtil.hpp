// Small file-system helpers shared by the importer, scene serializer and
// editor: whole-file reads/writes, UTF-8 <-> path/wide-string conversion
// (every path stored in engine data structures is a UTF-8 std::string, so
// non-ASCII folder names survive), and the executable's directory.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace Hollow {

std::string to_utf8(const std::filesystem::path& path);
std::filesystem::path path_from_utf8(std::string_view utf8);
std::wstring utf8_to_wide(std::string_view utf8);
std::string wide_to_utf8(std::wstring_view wide);

// Throws std::runtime_error if the file can't be opened.
std::string read_text_file(const std::filesystem::path& path);
std::vector<std::uint8_t> read_binary_file(const std::filesystem::path& path);
// Writes via a temporary file + rename so a crash mid-write never leaves a
// half-written scene behind. Throws on failure.
void write_text_file(const std::filesystem::path& path, std::string_view text);

// `path` relative to `base` when it lives underneath it (or nearby), else
// absolute. Always uses forward slashes. Used for portable scene files.
std::string make_relative_utf8(const std::filesystem::path& path, const std::filesystem::path& base);
// Inverse of make_relative_utf8: resolves `stored` against `base` if it is
// relative.
std::filesystem::path resolve_utf8(std::string_view stored, const std::filesystem::path& base);

// Directory containing the running executable (independent of the working
// directory, which varies with how the program was launched).
std::filesystem::path executable_directory();

} // namespace Hollow

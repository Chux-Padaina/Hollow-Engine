// Engine-wide logging. Messages go to stderr, the debugger output window
// (OutputDebugString), and an in-memory ring buffer that the editor's
// Console panel displays. Use with std::format:
//   log_warning(std::format("texture '{}' failed to load", path));
#pragma once

#include <cstdint>
#include <deque>
#include <string>
#include <string_view>

namespace Hollow {

enum class LogLevel {
    Info,
    Warning,
    Error,
};

struct LogEntry {
    LogLevel level = LogLevel::Info;
    std::string message;
    double seconds = 0.0; // since the first log call
};

void log_message(LogLevel level, std::string_view message);
inline void log_info(std::string_view message) { log_message(LogLevel::Info, message); }
inline void log_warning(std::string_view message) { log_message(LogLevel::Warning, message); }
inline void log_error(std::string_view message) { log_message(LogLevel::Error, message); }

// Most recent entries, oldest first (capped, see Log.cpp).
const std::deque<LogEntry>& log_entries();
// Increments every time an entry is added; lets UI auto-scroll on change.
std::uint64_t log_revision();
void clear_log();

} // namespace Hollow

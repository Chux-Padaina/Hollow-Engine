// Log ring buffer + sinks (stderr and the debugger via OutputDebugStringW).
#include "Hollow/Core/Log.hpp"

#include <Windows.h>

#include <chrono>
#include <cstdio>
#include <string>

namespace Hollow {
namespace {

constexpr std::size_t kMaxEntries = 512;

std::deque<LogEntry>& entries() {
    static std::deque<LogEntry> storage;
    return storage;
}

std::uint64_t g_revision = 0;

double seconds_since_start() {
    static const auto start = std::chrono::steady_clock::now();
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

const char* prefix(LogLevel level) {
    switch (level) {
    case LogLevel::Info:
        return "[info] ";
    case LogLevel::Warning:
        return "[warn] ";
    case LogLevel::Error:
        return "[error] ";
    }
    return "";
}

} // namespace

void log_message(LogLevel level, std::string_view message) {
    LogEntry entry;
    entry.level = level;
    entry.message = std::string(message);
    entry.seconds = seconds_since_start();

    const std::string line = std::string(prefix(level)) + entry.message + "\n";
    std::fputs(line.c_str(), stderr);
    const int wide_length = MultiByteToWideChar(CP_UTF8, 0, line.data(), static_cast<int>(line.size()), nullptr, 0);
    if (wide_length > 0) {
        std::wstring wide(static_cast<std::size_t>(wide_length), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, line.data(), static_cast<int>(line.size()), wide.data(), wide_length);
        OutputDebugStringW(wide.c_str());
    }

    auto& log = entries();
    log.push_back(std::move(entry));
    while (log.size() > kMaxEntries) {
        log.pop_front();
    }
    ++g_revision;
}

const std::deque<LogEntry>& log_entries() {
    return entries();
}

std::uint64_t log_revision() {
    return g_revision;
}

void clear_log() {
    entries().clear();
    ++g_revision;
}

} // namespace Hollow

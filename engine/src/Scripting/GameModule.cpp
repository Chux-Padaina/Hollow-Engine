// GameModuleHost: shadow-copy loading and hot reload of Game.dll.
#include "Hollow/Scripting/GameModule.hpp"

#include "Hollow/Core/FileUtil.hpp"
#include "Hollow/Core/Log.hpp"

#include <Windows.h>

#include <chrono>
#include <format>
#include <system_error>

namespace Hollow {

namespace {

constexpr const char* kModuleTag = "game";

double now_seconds() {
    using clock = std::chrono::steady_clock;
    return std::chrono::duration<double>(clock::now().time_since_epoch()).count();
}

std::filesystem::path shadow_directory() {
    std::error_code ec;
    std::filesystem::path dir = std::filesystem::temp_directory_path(ec) / L"HollowEngine" / L"GameModules";
    std::filesystem::create_directories(dir, ec);
    static bool cleaned = false;
    if (!cleaned) {
        // Leftovers from earlier sessions (a running editor's DLLs are locked
        // and simply survive this).
        cleaned = true;
        const std::wstring mine = std::to_wstring(GetCurrentProcessId()) + L"_";
        for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
            if (entry.is_directory(ec) && entry.path().filename().wstring().rfind(mine, 0) != 0) {
                std::filesystem::remove_all(entry.path(), ec);
            }
        }
    }
    return dir;
}

} // namespace

GameModuleHost::~GameModuleHost() {
    unload();
}

bool GameModuleHost::load(const std::filesystem::path& path) {
    path_ = path;
    std::error_code ec;
    if (!std::filesystem::exists(path_, ec)) {
        error_ = "not built yet";
        return false;
    }
    return load_now();
}

void GameModuleHost::unload() {
    if (library_ == nullptr) {
        return;
    }
    // Scripts reference code in the DLL: drop them first.
    ScriptRegistry::instance().remove_module(kModuleTag);
    FreeLibrary(static_cast<HMODULE>(library_));
    library_ = nullptr;
    std::error_code ec;
    std::filesystem::remove_all(shadow_.parent_path(), ec);
}

bool GameModuleHost::load_now() {
    std::error_code ec;
    const auto time = std::filesystem::last_write_time(path_, ec);
    const auto size = std::filesystem::file_size(path_, ec);
    if (ec) {
        error_ = ec.message();
        return false;
    }
    // Copy into a fresh folder per load: the compiler can overwrite the
    // original, Windows never confuses two loads of "the same" DLL, and the
    // PDB is copied alongside under its own name (the module is linked with
    // /PDBALTPATH:Game.pdb), so a debugger never locks the build's PDB.
    ++generation_;
    const std::filesystem::path folder = shadow_directory() / std::format(L"{}_{}", GetCurrentProcessId(), generation_);
    std::filesystem::create_directories(folder, ec);
    const std::filesystem::path shadow = folder / path_.filename();
    std::filesystem::copy_file(path_, shadow, std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) {
        error_ = "copy failed: " + ec.message();
        log_error(std::format("Game module: {}", error_));
        return false;
    }
    std::filesystem::path pdb = path_;
    pdb.replace_extension(".pdb");
    if (std::filesystem::exists(pdb, ec)) {
        std::filesystem::copy_file(pdb, folder / pdb.filename(), std::filesystem::copy_options::overwrite_existing, ec);
    }
    HMODULE library = LoadLibraryW(shadow.c_str());
    if (library == nullptr) {
        error_ = std::format("LoadLibrary failed (error {})", GetLastError());
        log_error(std::format("Game module '{}': {}", to_utf8(path_), error_));
        std::filesystem::remove_all(folder, ec);
        return false;
    }
    using VersionFn = int (*)();
    using RegisterFn = void (*)(ScriptRegistry&);
    auto version = reinterpret_cast<VersionFn>(reinterpret_cast<void*>(GetProcAddress(library, "hollow_game_api_version")));
    auto registrar = reinterpret_cast<RegisterFn>(reinterpret_cast<void*>(GetProcAddress(library, "hollow_game_register")));
    std::string problem;
    if (version == nullptr || registrar == nullptr) {
        problem = "not a game module (missing HOLLOW_GAME_MODULE)";
    } else if (version() != HOLLOW_GAME_API_VERSION) {
        problem = std::format("built for game API {} (engine is {}); rebuild it", version(), HOLLOW_GAME_API_VERSION);
    }
    if (!problem.empty()) {
        error_ = problem;
        log_error(std::format("Game module '{}': {}", to_utf8(path_), error_));
        FreeLibrary(library);
        std::filesystem::remove_all(folder, ec);
        return false;
    }
    ScriptRegistry& registry = ScriptRegistry::instance();
    registry.set_current_module(kModuleTag);
    try {
        registrar(registry);
    } catch (const std::exception& e) {
        log_error(std::format("Game module registration threw: {}", e.what()));
    }
    registry.set_current_module({});
    library_ = library;
    shadow_ = shadow;
    loaded_time_ = time;
    loaded_size_ = size;
    seen_time_ = time;
    pending_ = false;
    error_.clear();
    log_info(std::format("Game module loaded: {} ({} script{})", to_utf8(path_.filename()), script_count(), script_count() == 1 ? "" : "s"));
    return true;
}

bool GameModuleHost::reload() {
    if (path_.empty()) {
        return false;
    }
    std::error_code ec;
    if (!std::filesystem::exists(path_, ec)) {
        return false;
    }
    unload();
    return load_now();
}

bool GameModuleHost::poll(bool can_reload) {
    if (path_.empty()) {
        return false;
    }
    const double now = now_seconds();
    if (now - last_check_ < 0.5) {
        return pending_ && can_reload ? reload() : false;
    }
    last_check_ = now;
    std::error_code ec;
    const auto time = std::filesystem::last_write_time(path_, ec);
    if (ec) {
        return false; // being rebuilt (or never built)
    }
    if (library_ != nullptr && time == loaded_time_ && !pending_) {
        return false;
    }
    if (time != seen_time_) {
        // Wait until the linker has finished writing (stable for 0.5 s).
        seen_time_ = time;
        seen_since_ = now;
        return false;
    }
    if (now - seen_since_ < 0.5) {
        return false;
    }
    if (!can_reload) {
        if (!pending_) {
            pending_ = true;
            log_info("Game module rebuilt; it reloads when Play stops");
        }
        return false;
    }
    const bool was_loaded = library_ != nullptr;
    const bool ok = reload();
    if (ok && was_loaded) {
        log_info("Game module hot-reloaded");
    }
    return ok;
}

std::size_t GameModuleHost::script_count() const {
    return ScriptRegistry::instance().count_module(kModuleTag);
}

} // namespace Hollow

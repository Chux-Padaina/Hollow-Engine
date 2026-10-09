// Game modules: gameplay code built as a DLL (Game.dll) that the editor
// loads at startup and hot-reloads whenever it's rebuilt.
//
// Writing a module (see engine/game/ for the sample):
//
//   #include "Hollow/Scripting/GameModule.hpp"
//
//   class Spinner : public Hollow::Script { ... };
//
//   HOLLOW_GAME_MODULE(registry) {
//       registry.add({"Spinner", "Spins", {{"speed", 1.0f}},
//                     [] { return std::make_unique<Spinner>(); }});
//   }
//
// The module links against HollowRuntime (the engine runtime DLL), so
// scripts use the same Scene, Actor, PhysicsWorld, Input, Log... as the
// editor. Rebuild the HollowGame target while the editor runs: once the new
// DLL is written, the editor swaps it in (after Play stops, if playing).
// Script components keep their names and parameter values across reloads;
// script *instances* only exist in Play mode, so no state needs migrating.
#pragma once

#include "Hollow/Scripting/Script.hpp"

#include <filesystem>
#include <string>

// Bumped whenever the engine <-> module contract changes. A module built
// against a different version is refused instead of crashing.
#define HOLLOW_GAME_API_VERSION 1

#if defined(_WIN32)
#define HOLLOW_GAME_EXPORT extern "C" __declspec(dllexport)
#else
#define HOLLOW_GAME_EXPORT extern "C" __attribute__((visibility("default")))
#endif

// Defines the module's entry points. The body registers scripts:
//   HOLLOW_GAME_MODULE(registry) { registry.add({...}); }
#define HOLLOW_GAME_MODULE(registry_name)                                          \
    HOLLOW_GAME_EXPORT int hollow_game_api_version() { return HOLLOW_GAME_API_VERSION; } \
    static void hollow_game_register_impl(::Hollow::ScriptRegistry& registry_name); \
    HOLLOW_GAME_EXPORT void hollow_game_register(::Hollow::ScriptRegistry& registry) { \
        hollow_game_register_impl(registry);                                       \
    }                                                                              \
    static void hollow_game_register_impl(::Hollow::ScriptRegistry& registry_name)

namespace Hollow {

// Loads a game module through a shadow copy (so the original stays
// writable for the compiler) and reloads it when the file changes.
class GameModuleHost {
public:
    GameModuleHost() = default;
    ~GameModuleHost();
    GameModuleHost(const GameModuleHost&) = delete;
    GameModuleHost& operator=(const GameModuleHost&) = delete;

    // Remembers `path` and loads it if it exists. Returns true if loaded.
    bool load(const std::filesystem::path& path);
    void unload();
    // Checks for a rebuilt DLL (rate-limited). Reloads only when `can_reload`
    // (i.e. not in Play mode); otherwise it waits. Returns true on reload.
    bool poll(bool can_reload);
    // Forces a reload now (if allowed).
    bool reload();

    bool loaded() const { return library_ != nullptr; }
    bool reload_pending() const { return pending_; }
    const std::filesystem::path& path() const { return path_; }
    std::size_t script_count() const;
    const std::string& last_error() const { return error_; }
    int generation() const { return generation_; }

private:
    bool load_now();

    std::filesystem::path path_;
    std::filesystem::path shadow_;
    void* library_ = nullptr;
    std::filesystem::file_time_type loaded_time_{};
    std::uintmax_t loaded_size_ = 0;
    std::filesystem::file_time_type seen_time_{};
    double seen_since_ = 0.0;
    double last_check_ = 0.0;
    bool pending_ = false;
    int generation_ = 0;
    std::string error_;
};

} // namespace Hollow

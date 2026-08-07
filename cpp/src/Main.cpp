// Executable entry point: forwards command-line arguments as startup asset
// imports and hands off to Hollow::App.
#include "Hollow/App.hpp"

#include <filesystem>
#include <vector>

int main(int argc, char** argv) {
    std::vector<std::filesystem::path> startup_imports;
    for (int i = 1; i < argc; ++i) {
        startup_imports.emplace_back(argv[i]);
    }
    Hollow::App app;
    return app.run(startup_imports);
}

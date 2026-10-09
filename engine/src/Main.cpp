// Executable entry point: forwards command-line arguments (models, .hdr
// environments or a .hscene to open) to the editor.
#include "Hollow/Editor/App.hpp"

#include <Windows.h>
#include <shellapi.h>

#include <filesystem>
#include <vector>

int main() {
    // Read the wide command line so non-ASCII paths survive (argv is in the
    // ANSI code page on Windows).
    std::vector<std::filesystem::path> startup_files;
    int argc = 0;
    if (LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc)) {
        for (int i = 1; i < argc; ++i) {
            startup_files.emplace_back(argv[i]);
        }
        LocalFree(argv);
    }
    Hollow::App app;
    return app.run(startup_files);
}

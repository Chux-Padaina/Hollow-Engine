// Material textures: loaded by path, decoded and block-compressed on a
// worker thread, cached on disk (%LOCALAPPDATA%/HollowEngine/TextureCache)
// so the second load is just a file read, and uploaded on the render
// thread within a per-frame budget. Until a texture is ready, get() returns
// a neutral default for its usage, so the scene renders immediately.
#pragma once

#include "Render/TextureCodec.hpp"

#include <glad/gl.h>

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace Hollow {

class TextureManager {
public:
    TextureManager();
    ~TextureManager();
    TextureManager(const TextureManager&) = delete;
    TextureManager& operator=(const TextureManager&) = delete;

    void initialize();
    void shutdown();

    // GL texture for `path` (UTF-8). Starts loading on first use.
    GLuint get(const std::string& path, texcodec::Usage usage);
    // Neutral stand-ins: white, flat normal, black.
    GLuint default_texture(texcodec::Usage usage) const;
    GLuint white() const { return white_; }
    GLuint black() const { return black_; }

    // Uploads decoded textures (main thread). `budget_bytes` limits the
    // work per frame (0 = everything that's ready).
    void update(std::size_t budget_bytes);
    // Settings; changing compression reloads textures as they're used.
    void set_async(bool async) { async_ = async; }
    void set_compression(bool compression);

    std::size_t texture_count() const { return entries_.size(); }
    std::size_t gpu_bytes() const { return gpu_bytes_; }
    std::size_t pending() const;
    // Synchronously loads one image into a new GL texture (decals,
    // particles, LUT sprites). Returns 0 on failure.
    GLuint load_now(const std::string& path, texcodec::Usage usage);

private:
    struct Entry {
        GLuint id = 0;
        bool failed = false;
        bool queued = false;
        std::size_t bytes = 0;
    };
    struct Job {
        std::string key;
        std::string path;
        texcodec::Usage usage;
        bool compress;
    };
    struct Result {
        std::string key;
        texcodec::TextureData data;
        bool ok = false;
        std::string error;
    };

    static std::string make_key(const std::string& path, texcodec::Usage usage);
    void worker_loop();
    static Result process(const Job& job);
    GLuint upload(const texcodec::TextureData& data, std::size_t& bytes) const;

    std::unordered_map<std::string, Entry> entries_;
    GLuint white_ = 0;
    GLuint black_ = 0;
    GLuint flat_normal_ = 0;
    bool async_ = true;
    bool compression_ = true;
    std::size_t gpu_bytes_ = 0;

    std::thread worker_;
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<Job> jobs_;
    std::vector<Result> results_;
    bool stop_ = false;
    bool initialized_ = false;
};

} // namespace Hollow

// Implementation of TextureManager.hpp.
#include "Render/TextureManager.hpp"

#include "Hollow/Assets/TextureLoader.hpp"
#include "Hollow/Core/FileUtil.hpp"
#include "Hollow/Core/Log.hpp"

#include <Windows.h>
#include <objbase.h>

#include <algorithm>
#include <cctype>
#include <format>
#include <functional>

namespace Hollow {

using texcodec::Usage;

namespace {

std::filesystem::path cache_directory() {
    wchar_t buffer[MAX_PATH] = {};
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", buffer, MAX_PATH) > 0) {
        return std::filesystem::path(buffer) / "HollowEngine" / "TextureCache";
    }
    return std::filesystem::temp_directory_path() / "HollowEngine" / "TextureCache";
}

std::string lower_extension(const std::filesystem::path& path) {
    std::string ext = to_utf8(path.extension());
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext;
}

GLuint solid_texture(std::uint8_t r, std::uint8_t g, std::uint8_t b, std::uint8_t a) {
    GLuint id = 0;
    glCreateTextures(GL_TEXTURE_2D, 1, &id);
    glTextureStorage2D(id, 1, GL_RGBA8, 1, 1);
    const std::uint8_t pixel[4] = {r, g, b, a};
    glTextureSubImage2D(id, 0, 0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
    return id;
}

} // namespace

TextureManager::TextureManager() = default;

TextureManager::~TextureManager() {
    shutdown();
}

void TextureManager::initialize() {
    if (initialized_) {
        return;
    }
    white_ = solid_texture(255, 255, 255, 255);
    black_ = solid_texture(0, 0, 0, 255);
    flat_normal_ = solid_texture(128, 128, 255, 255);
    stop_ = false;
    worker_ = std::thread([this] { worker_loop(); });
    initialized_ = true;
}

void TextureManager::shutdown() {
    if (!initialized_) {
        return;
    }
    {
        std::lock_guard lock(mutex_);
        stop_ = true;
        jobs_.clear();
    }
    wake_.notify_all();
    if (worker_.joinable()) {
        worker_.join();
    }
    for (auto& [key, entry] : entries_) {
        if (entry.id != 0) {
            glDeleteTextures(1, &entry.id);
        }
    }
    entries_.clear();
    results_.clear();
    for (GLuint* id : {&white_, &black_, &flat_normal_}) {
        if (*id != 0) {
            glDeleteTextures(1, id);
            *id = 0;
        }
    }
    gpu_bytes_ = 0;
    initialized_ = false;
}

std::string TextureManager::make_key(const std::string& path, Usage usage) {
    return path + "|" + std::to_string(static_cast<int>(usage));
}

GLuint TextureManager::default_texture(Usage usage) const {
    return usage == Usage::Normal ? flat_normal_ : white_;
}

void TextureManager::set_compression(bool compression) {
    if (compression == compression_) {
        return;
    }
    compression_ = compression;
    // Reload everything in the new format as it's used again.
    for (auto& [key, entry] : entries_) {
        if (entry.id != 0) {
            glDeleteTextures(1, &entry.id);
        }
    }
    entries_.clear();
    gpu_bytes_ = 0;
    std::lock_guard lock(mutex_);
    jobs_.clear();
    results_.clear();
}

std::size_t TextureManager::pending() const {
    std::lock_guard lock(mutex_);
    return jobs_.size() + results_.size();
}

GLuint TextureManager::get(const std::string& path, Usage usage) {
    if (path.empty()) {
        return default_texture(usage);
    }
    const std::string key = make_key(path, usage);
    auto [it, inserted] = entries_.try_emplace(key);
    Entry& entry = it->second;
    if (entry.id != 0) {
        return entry.id;
    }
    if (entry.failed) {
        return default_texture(usage);
    }
    if (!entry.queued) {
        entry.queued = true;
        Job job{key, path, usage, compression_};
        if (async_) {
            {
                std::lock_guard lock(mutex_);
                jobs_.push_back(std::move(job));
            }
            wake_.notify_one();
        } else {
            Result result = process(job);
            std::lock_guard lock(mutex_);
            results_.push_back(std::move(result));
        }
    }
    if (!async_) {
        update(0);
        if (entry.id != 0) {
            return entry.id;
        }
    }
    return default_texture(usage);
}

void TextureManager::worker_loop() {
    // The worker owns its COM apartment (WIC decoding).
    const HRESULT coinit = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    while (true) {
        Job job;
        {
            std::unique_lock lock(mutex_);
            wake_.wait(lock, [this] { return stop_ || !jobs_.empty(); });
            if (stop_) {
                break;
            }
            job = std::move(jobs_.front());
            jobs_.pop_front();
        }
        Result result = process(job);
        std::lock_guard lock(mutex_);
        if (!stop_) {
            results_.push_back(std::move(result));
        }
    }
    if (SUCCEEDED(coinit)) {
        CoUninitialize();
    }
}

TextureManager::Result TextureManager::process(const Job& job) {
    Result result;
    result.key = job.key;
    try {
        const std::filesystem::path path = path_from_utf8(job.path);
        const std::string ext = lower_extension(path);
        const bool srgb = job.usage == Usage::Color;
        if (ext == ".dds") {
            result.data = texcodec::load_dds(path, srgb);
            result.ok = true;
            return result;
        }
        if (ext == ".ktx2") {
            result.data = texcodec::load_ktx2(path, srgb);
            result.ok = true;
            return result;
        }
        std::error_code ec;
        const std::uint64_t size = std::filesystem::file_size(path, ec);
        const std::int64_t time = static_cast<std::int64_t>(std::filesystem::last_write_time(path, ec).time_since_epoch().count());
        std::filesystem::path cache_file;
        if (job.compress) {
            const std::size_t hash = std::hash<std::string>{}(job.key);
            cache_file = cache_directory() / std::format("{:016x}.htex", static_cast<std::uint64_t>(hash));
            if (texcodec::read_cache(cache_file, size, time, result.data)) {
                result.ok = true;
                return result;
            }
        }
        TextureImage image = load_texture_image(path);
        texcodec::TextureData data = texcodec::build_mips(image.width, image.height, std::move(image.rgba), job.usage);
        if (job.compress && image.width >= 4 && image.height >= 4) {
            data = texcodec::compress(data, job.usage);
            texcodec::write_cache(cache_file, size, time, data);
        }
        result.data = std::move(data);
        result.ok = true;
    } catch (const std::exception& error) {
        result.ok = false;
        result.error = error.what();
    }
    return result;
}

GLuint TextureManager::upload(const texcodec::TextureData& data, std::size_t& bytes) const {
    if (data.mips.empty()) {
        return 0;
    }
    GLuint id = 0;
    glCreateTextures(GL_TEXTURE_2D, 1, &id);
    const auto& base = data.mips.front();
    glTextureStorage2D(id, static_cast<GLsizei>(data.mips.size()), data.internal_format, base.width, base.height);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    bytes = 0;
    for (std::size_t level = 0; level < data.mips.size(); ++level) {
        const auto& mip = data.mips[level];
        if (data.compressed) {
            glCompressedTextureSubImage2D(id, static_cast<GLint>(level), 0, 0, mip.width, mip.height, data.internal_format, static_cast<GLsizei>(mip.data.size()), mip.data.data());
        } else {
            glTextureSubImage2D(id, static_cast<GLint>(level), 0, 0, mip.width, mip.height, GL_RGBA, GL_UNSIGNED_BYTE, mip.data.data());
        }
        bytes += mip.data.size();
    }
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTextureParameteri(id, GL_TEXTURE_MAX_LEVEL, static_cast<GLint>(data.mips.size()) - 1);
    return id;
}

void TextureManager::update(std::size_t budget_bytes) {
    std::vector<Result> ready;
    {
        std::lock_guard lock(mutex_);
        if (results_.empty()) {
            return;
        }
        std::size_t used = 0;
        std::size_t take = 0;
        for (; take < results_.size(); ++take) {
            if (budget_bytes > 0 && take > 0 && used >= budget_bytes) {
                break;
            }
            used += results_[take].data.bytes();
        }
        ready.assign(std::make_move_iterator(results_.begin()), std::make_move_iterator(results_.begin() + static_cast<std::ptrdiff_t>(take)));
        results_.erase(results_.begin(), results_.begin() + static_cast<std::ptrdiff_t>(take));
    }
    for (Result& result : ready) {
        auto it = entries_.find(result.key);
        if (it == entries_.end()) {
            continue; // cleared meanwhile
        }
        Entry& entry = it->second;
        entry.queued = false;
        if (!result.ok) {
            entry.failed = true;
            const std::string path = result.key.substr(0, result.key.rfind('|'));
            log_warning(std::format("Texture '{}' failed to load: {}", path, result.error));
            continue;
        }
        entry.id = upload(result.data, entry.bytes);
        gpu_bytes_ += entry.bytes;
    }
}

GLuint TextureManager::load_now(const std::string& path, Usage usage) {
    Result result = process({make_key(path, usage), path, usage, false});
    if (!result.ok) {
        log_warning(std::format("Texture '{}' failed to load: {}", path, result.error));
        return 0;
    }
    std::size_t bytes = 0;
    return upload(result.data, bytes);
}

} // namespace Hollow

// A frame render graph. Every frame the renderer declares its passes and
// the textures/buffers each one reads and writes; the graph then
//
//   * culls passes whose results nobody uses (a pass survives if it writes
//     an imported resource, is marked side_effect(), or feeds such a pass),
//   * allocates the transient textures from a pool and aliases them: a
//     texture whose last reader has run goes back to the pool and can back a
//     later resource with the same description in the same frame,
//   * inserts glMemoryBarrier calls between passes that write through image
//     stores / SSBOs and passes that consume those writes,
//   * times every pass on the GPU (timestamp queries) and labels it for GPU
//     debuggers (RenderDoc, Nsight).
//
// Persistent resources (TAA history, shadow atlas, ...) are owned by the
// renderer and imported each frame.
#pragma once

#include "Render/Gfx.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace Hollow::rg {

struct TextureHandle {
    std::uint32_t index = 0xFFFFFFFFu;
    bool valid() const { return index != 0xFFFFFFFFu; }
    explicit operator bool() const { return valid(); }
};

struct BufferHandle {
    std::uint32_t index = 0xFFFFFFFFu;
    bool valid() const { return index != 0xFFFFFFFFu; }
    explicit operator bool() const { return valid(); }
};

enum class Access : std::uint8_t {
    Sampled,      // texture fetch
    Image,        // image load/store
    RenderTarget, // color attachment
    Depth,        // depth attachment (test and/or write)
    Storage,      // SSBO
    Indirect,     // indirect draw/dispatch arguments
    Uniform,
    Vertex,       // vertex/index buffer
    Transfer,     // copies, clears, blits
};

class RenderGraph;

class PassBuilder {
public:
    TextureHandle create(const char* name, const gfx::TextureDesc& desc);
    BufferHandle create_buffer(const char* name, std::size_t size);
    TextureHandle read(TextureHandle handle, Access access = Access::Sampled);
    TextureHandle write(TextureHandle handle, Access access = Access::RenderTarget);
    BufferHandle read(BufferHandle handle, Access access = Access::Storage);
    BufferHandle write(BufferHandle handle, Access access = Access::Storage);
    void side_effect();
    const gfx::TextureDesc& desc(TextureHandle handle) const;

private:
    friend class RenderGraph;
    PassBuilder(RenderGraph& graph, std::uint32_t pass) : graph_(graph), pass_(pass) {}
    RenderGraph& graph_;
    std::uint32_t pass_;
};

struct Target {
    TextureHandle texture;
    int level = 0;
    int layer = -1;
};

class PassContext {
public:
    const gfx::Texture& texture(TextureHandle handle) const;
    GLuint tex(TextureHandle handle) const { return texture(handle).id(); }
    const gfx::Buffer& buffer(BufferHandle handle) const;
    GLuint buf(BufferHandle handle) const { return buffer(handle).id(); }
    // Binds an FBO for these attachments and sets the viewport to the
    // first attachment's (mip) size.
    void bind_targets(std::initializer_list<Target> colors, Target depth = {}) const;
    // Same, with explicit viewport.
    void bind_targets_viewport(std::initializer_list<Target> colors, Target depth, int x, int y, int width, int height) const;
    gfx::FramebufferCache& framebuffers() const;

private:
    friend class RenderGraph;
    explicit PassContext(RenderGraph& graph) : graph_(graph) {}
    RenderGraph& graph_;
};

struct PassInfo {
    std::string name;
    bool culled = false;
    std::vector<std::string> reads;
    std::vector<std::string> writes;
    double gpu_ms = 0.0;
};

struct ResourceInfo {
    std::string name;
    gfx::TextureDesc desc;
    bool imported = false;
    bool buffer = false;
    std::size_t buffer_size = 0;
    int physical = -1; // pool slot (shared slots = aliased memory)
    int first_pass = -1;
    int last_pass = -1;
};

class RenderGraph {
public:
    explicit RenderGraph(gfx::FramebufferCache& framebuffers, gfx::GpuProfiler* profiler);
    ~RenderGraph();
    RenderGraph(const RenderGraph&) = delete;
    RenderGraph& operator=(const RenderGraph&) = delete;

    // Starts a new frame's graph (the texture pool persists).
    void reset();

    TextureHandle import(const char* name, const gfx::Texture& texture);
    BufferHandle import(const char* name, const gfx::Buffer& buffer);

    template <typename Data>
    const Data& add_pass(const char* name, const std::function<void(PassBuilder&, Data&)>& setup,
        std::function<void(const Data&, const PassContext&)> execute) {
        auto pass = std::make_unique<TypedPass<Data>>();
        pass->name = name;
        pass->execute_fn = std::move(execute);
        const auto index = static_cast<std::uint32_t>(passes_.size());
        Data& data = pass->data;
        passes_.push_back(std::move(pass));
        PassBuilder builder(*this, index);
        setup(builder, data);
        return data;
    }

    // A pass with no data struct.
    void add_pass(const char* name, const std::function<void(PassBuilder&)>& setup, std::function<void(const PassContext&)> execute);

    void compile();
    void execute();

    const gfx::TextureDesc& desc(TextureHandle handle) const;
    std::vector<PassInfo> pass_info() const;
    std::vector<ResourceInfo> resource_info() const;
    std::size_t pooled_texture_count() const { return pool_.size(); }
    std::size_t pooled_texture_bytes() const;
    // Frees pooled textures unused for a while.
    void trim(int max_idle_frames = 60);
    void clear_pool();

private:
    friend class PassBuilder;
    friend class PassContext;

    struct Pass {
        virtual ~Pass() = default;
        virtual void run(const PassContext& context) = 0;
        std::string name;
        struct Use {
            bool buffer;
            std::uint32_t index;
            Access access;
            bool write;
        };
        std::vector<Use> uses;
        bool side_effect = false;
        bool culled = false;
        double gpu_ms = 0.0;
    };
    template <typename Data>
    struct TypedPass final : Pass {
        Data data{};
        std::function<void(const Data&, const PassContext&)> execute_fn;
        void run(const PassContext& context) override { execute_fn(data, context); }
    };
    struct PlainPass final : Pass {
        std::function<void(const PassContext&)> execute_fn;
        void run(const PassContext& context) override { execute_fn(context); }
    };
    struct TextureResource {
        std::string name;
        gfx::TextureDesc desc;
        const gfx::Texture* imported = nullptr;
        int physical = -1;
        int first = -1;
        int last = -1;
        bool pending_incoherent_write = false;
    };
    struct BufferResource {
        std::string name;
        std::size_t size = 0;
        const gfx::Buffer* imported = nullptr;
        int physical = -1;
        int first = -1;
        int last = -1;
        bool pending_incoherent_write = false;
    };
    struct PooledTexture {
        gfx::Texture texture;
        bool in_use = false;
        std::uint64_t last_used = 0;
    };
    struct PooledBuffer {
        gfx::Buffer buffer;
        bool in_use = false;
        std::uint64_t last_used = 0;
    };

    void record(std::uint32_t pass, bool buffer, std::uint32_t index, Access access, bool write);
    int acquire_texture(const gfx::TextureDesc& desc, const std::string& name);
    int acquire_buffer(std::size_t size, const std::string& name);
    const gfx::Texture& resolve(TextureHandle handle) const;
    const gfx::Buffer& resolve(BufferHandle handle) const;
    void emit_barriers(const Pass& pass);

    gfx::FramebufferCache& framebuffers_;
    gfx::GpuProfiler* profiler_;
    std::vector<std::unique_ptr<Pass>> passes_;
    std::vector<TextureResource> textures_;
    std::vector<BufferResource> buffers_;
    std::vector<PooledTexture> pool_;
    std::vector<PooledBuffer> buffer_pool_;
    std::vector<std::vector<std::uint32_t>> release_after_; // per pass: texture indices
    std::vector<std::vector<std::uint32_t>> release_buffers_after_;
    std::vector<std::vector<std::uint32_t>> acquire_before_;
    std::vector<std::vector<std::uint32_t>> acquire_buffers_before_;
    std::uint64_t frame_ = 0;
    bool compiled_ = false;
    // Pass timings from earlier frames, by name.
    std::vector<std::pair<std::string, double>> last_timings_;
};

} // namespace Hollow::rg

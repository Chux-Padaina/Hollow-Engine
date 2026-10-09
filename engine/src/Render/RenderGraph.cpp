// Implementation of RenderGraph.hpp.
#include "Render/RenderGraph.hpp"

#include "Hollow/Core/Log.hpp"

#include <algorithm>
#include <stdexcept>

namespace Hollow::rg {

namespace {

bool incoherent(Access access) {
    return access == Access::Image || access == Access::Storage;
}

GLbitfield barrier_for(Access access) {
    switch (access) {
    case Access::Sampled: return GL_TEXTURE_FETCH_BARRIER_BIT;
    case Access::Image: return GL_SHADER_IMAGE_ACCESS_BARRIER_BIT;
    case Access::RenderTarget:
    case Access::Depth: return GL_FRAMEBUFFER_BARRIER_BIT;
    case Access::Storage: return GL_SHADER_STORAGE_BARRIER_BIT;
    case Access::Indirect: return GL_COMMAND_BARRIER_BIT;
    case Access::Uniform: return GL_UNIFORM_BARRIER_BIT;
    case Access::Vertex: return GL_VERTEX_ATTRIB_ARRAY_BARRIER_BIT | GL_ELEMENT_ARRAY_BARRIER_BIT;
    case Access::Transfer: return GL_BUFFER_UPDATE_BARRIER_BIT | GL_TEXTURE_UPDATE_BARRIER_BIT | GL_PIXEL_BUFFER_BARRIER_BIT;
    }
    return GL_ALL_BARRIER_BITS;
}

} // namespace

// --- PassBuilder --------------------------------------------------------------

TextureHandle PassBuilder::create(const char* name, const gfx::TextureDesc& desc) {
    RenderGraph::TextureResource resource;
    resource.name = name;
    resource.desc = desc;
    graph_.textures_.push_back(std::move(resource));
    return TextureHandle{static_cast<std::uint32_t>(graph_.textures_.size() - 1)};
}

BufferHandle PassBuilder::create_buffer(const char* name, std::size_t size) {
    RenderGraph::BufferResource resource;
    resource.name = name;
    resource.size = std::max<std::size_t>(size, 16);
    graph_.buffers_.push_back(std::move(resource));
    return BufferHandle{static_cast<std::uint32_t>(graph_.buffers_.size() - 1)};
}

TextureHandle PassBuilder::read(TextureHandle handle, Access access) {
    if (handle.valid()) {
        graph_.record(pass_, false, handle.index, access, false);
    }
    return handle;
}

TextureHandle PassBuilder::write(TextureHandle handle, Access access) {
    if (handle.valid()) {
        graph_.record(pass_, false, handle.index, access, true);
    }
    return handle;
}

BufferHandle PassBuilder::read(BufferHandle handle, Access access) {
    if (handle.valid()) {
        graph_.record(pass_, true, handle.index, access, false);
    }
    return handle;
}

BufferHandle PassBuilder::write(BufferHandle handle, Access access) {
    if (handle.valid()) {
        graph_.record(pass_, true, handle.index, access, true);
    }
    return handle;
}

void PassBuilder::side_effect() {
    graph_.passes_[pass_]->side_effect = true;
}

const gfx::TextureDesc& PassBuilder::desc(TextureHandle handle) const {
    return graph_.desc(handle);
}

// --- PassContext ----------------------------------------------------------------

const gfx::Texture& PassContext::texture(TextureHandle handle) const {
    return graph_.resolve(handle);
}

const gfx::Buffer& PassContext::buffer(BufferHandle handle) const {
    return graph_.resolve(handle);
}

gfx::FramebufferCache& PassContext::framebuffers() const {
    return graph_.framebuffers_;
}

void PassContext::bind_targets(std::initializer_list<Target> colors, Target depth) const {
    int width = 1;
    int height = 1;
    const Target* first = colors.size() > 0 ? colors.begin() : (depth.texture.valid() ? &depth : nullptr);
    if (first != nullptr) {
        const gfx::TextureDesc& d = graph_.resolve(first->texture).desc();
        width = std::max(1, d.width >> first->level);
        height = std::max(1, d.height >> first->level);
    }
    bind_targets_viewport(colors, depth, 0, 0, width, height);
}

void PassContext::bind_targets_viewport(std::initializer_list<Target> colors, Target depth, int x, int y, int width, int height) const {
    std::vector<gfx::Attachment> attachments;
    attachments.reserve(colors.size());
    for (const Target& target : colors) {
        attachments.push_back({graph_.resolve(target.texture).id(), target.level, target.layer});
    }
    gfx::Attachment depth_attachment;
    if (depth.texture.valid()) {
        depth_attachment = {graph_.resolve(depth.texture).id(), depth.level, depth.layer};
    }
    glBindFramebuffer(GL_FRAMEBUFFER, graph_.framebuffers_.get(attachments, depth_attachment));
    glViewport(x, y, width, height);
}

// --- RenderGraph -----------------------------------------------------------------

RenderGraph::RenderGraph(gfx::FramebufferCache& framebuffers, gfx::GpuProfiler* profiler) : framebuffers_(framebuffers), profiler_(profiler) {}

RenderGraph::~RenderGraph() {
    clear_pool();
}

void RenderGraph::reset() {
    // Keep the previous frame's timings for pass_info().
    last_timings_.clear();
    if (profiler_ != nullptr) {
        for (const auto& result : profiler_->results()) {
            last_timings_.emplace_back(result.name, result.milliseconds);
        }
    }
    passes_.clear();
    textures_.clear();
    buffers_.clear();
    release_after_.clear();
    release_buffers_after_.clear();
    acquire_before_.clear();
    acquire_buffers_before_.clear();
    for (PooledTexture& pooled : pool_) {
        pooled.in_use = false;
    }
    for (PooledBuffer& pooled : buffer_pool_) {
        pooled.in_use = false;
    }
    compiled_ = false;
    ++frame_;
}

TextureHandle RenderGraph::import(const char* name, const gfx::Texture& texture) {
    TextureResource resource;
    resource.name = name;
    resource.desc = texture.desc();
    resource.imported = &texture;
    textures_.push_back(std::move(resource));
    return TextureHandle{static_cast<std::uint32_t>(textures_.size() - 1)};
}

BufferHandle RenderGraph::import(const char* name, const gfx::Buffer& buffer) {
    BufferResource resource;
    resource.name = name;
    resource.size = buffer.size();
    resource.imported = &buffer;
    buffers_.push_back(std::move(resource));
    return BufferHandle{static_cast<std::uint32_t>(buffers_.size() - 1)};
}

void RenderGraph::add_pass(const char* name, const std::function<void(PassBuilder&)>& setup, std::function<void(const PassContext&)> execute) {
    auto pass = std::make_unique<PlainPass>();
    pass->name = name;
    pass->execute_fn = std::move(execute);
    const auto index = static_cast<std::uint32_t>(passes_.size());
    passes_.push_back(std::move(pass));
    PassBuilder builder(*this, index);
    setup(builder);
}

void RenderGraph::record(std::uint32_t pass, bool buffer, std::uint32_t index, Access access, bool write) {
    passes_[pass]->uses.push_back({buffer, index, access, write});
}

const gfx::TextureDesc& RenderGraph::desc(TextureHandle handle) const {
    return textures_.at(handle.index).desc;
}

void RenderGraph::compile() {
    const std::size_t pass_count = passes_.size();
    // Culling: walk backwards; a pass is live if it has a side effect,
    // writes an imported resource, or writes something a live pass reads.
    std::vector<bool> texture_needed(textures_.size(), false);
    std::vector<bool> buffer_needed(buffers_.size(), false);
    for (std::size_t p = pass_count; p-- > 0;) {
        Pass& pass = *passes_[p];
        bool live = pass.side_effect;
        for (const Pass::Use& use : pass.uses) {
            if (!use.write) {
                continue;
            }
            if (use.buffer) {
                const BufferResource& r = buffers_[use.index];
                live = live || r.imported != nullptr || buffer_needed[use.index];
            } else {
                const TextureResource& r = textures_[use.index];
                live = live || r.imported != nullptr || texture_needed[use.index];
            }
        }
        pass.culled = !live;
        if (live) {
            for (const Pass::Use& use : pass.uses) {
                if (!use.write) {
                    (use.buffer ? buffer_needed : texture_needed)[use.index] = true;
                }
            }
        }
    }

    // Lifetimes over live passes.
    for (std::size_t p = 0; p < pass_count; ++p) {
        if (passes_[p]->culled) {
            continue;
        }
        for (const Pass::Use& use : passes_[p]->uses) {
            int* first = use.buffer ? &buffers_[use.index].first : &textures_[use.index].first;
            int* last = use.buffer ? &buffers_[use.index].last : &textures_[use.index].last;
            if (*first < 0) {
                *first = static_cast<int>(p);
            }
            *last = static_cast<int>(p);
        }
    }

    acquire_before_.assign(pass_count, {});
    release_after_.assign(pass_count, {});
    acquire_buffers_before_.assign(pass_count, {});
    release_buffers_after_.assign(pass_count, {});
    for (std::uint32_t i = 0; i < textures_.size(); ++i) {
        const TextureResource& r = textures_[i];
        if (r.imported != nullptr || r.first < 0) {
            continue;
        }
        acquire_before_[static_cast<std::size_t>(r.first)].push_back(i);
        release_after_[static_cast<std::size_t>(r.last)].push_back(i);
    }
    for (std::uint32_t i = 0; i < buffers_.size(); ++i) {
        const BufferResource& r = buffers_[i];
        if (r.imported != nullptr || r.first < 0) {
            continue;
        }
        acquire_buffers_before_[static_cast<std::size_t>(r.first)].push_back(i);
        release_buffers_after_[static_cast<std::size_t>(r.last)].push_back(i);
    }
    compiled_ = true;
}

int RenderGraph::acquire_texture(const gfx::TextureDesc& desc, const std::string& name) {
    for (std::size_t i = 0; i < pool_.size(); ++i) {
        PooledTexture& pooled = pool_[i];
        if (!pooled.in_use && pooled.texture.desc() == desc) {
            pooled.in_use = true;
            pooled.last_used = frame_;
            return static_cast<int>(i);
        }
    }
    PooledTexture pooled;
    pooled.texture = gfx::Texture(desc, name.c_str());
    pooled.in_use = true;
    pooled.last_used = frame_;
    pool_.push_back(std::move(pooled));
    return static_cast<int>(pool_.size() - 1);
}

int RenderGraph::acquire_buffer(std::size_t size, const std::string& name) {
    for (std::size_t i = 0; i < buffer_pool_.size(); ++i) {
        PooledBuffer& pooled = buffer_pool_[i];
        if (!pooled.in_use && pooled.buffer.size() >= size && pooled.buffer.size() <= size * 2) {
            pooled.in_use = true;
            pooled.last_used = frame_;
            return static_cast<int>(i);
        }
    }
    PooledBuffer pooled;
    pooled.buffer = gfx::Buffer(size, GL_DYNAMIC_STORAGE_BIT, nullptr, name.c_str());
    pooled.in_use = true;
    pooled.last_used = frame_;
    buffer_pool_.push_back(std::move(pooled));
    return static_cast<int>(buffer_pool_.size() - 1);
}

const gfx::Texture& RenderGraph::resolve(TextureHandle handle) const {
    const TextureResource& r = textures_.at(handle.index);
    if (r.imported != nullptr) {
        return *r.imported;
    }
    if (r.physical < 0) {
        throw std::logic_error("render graph texture '" + r.name + "' used outside its declared lifetime");
    }
    return pool_[static_cast<std::size_t>(r.physical)].texture;
}

const gfx::Buffer& RenderGraph::resolve(BufferHandle handle) const {
    const BufferResource& r = buffers_.at(handle.index);
    if (r.imported != nullptr) {
        return *r.imported;
    }
    if (r.physical < 0) {
        throw std::logic_error("render graph buffer '" + r.name + "' used outside its declared lifetime");
    }
    return buffer_pool_[static_cast<std::size_t>(r.physical)].buffer;
}

void RenderGraph::emit_barriers(const Pass& pass) {
    GLbitfield bits = 0;
    for (const Pass::Use& use : pass.uses) {
        bool* pending = use.buffer ? &buffers_[use.index].pending_incoherent_write : &textures_[use.index].pending_incoherent_write;
        if (*pending) {
            bits |= barrier_for(use.access);
            if (!use.write || !incoherent(use.access)) {
                *pending = false;
            }
        }
    }
    if (bits != 0) {
        glMemoryBarrier(bits);
    }
    for (const Pass::Use& use : pass.uses) {
        if (use.write && incoherent(use.access)) {
            (use.buffer ? buffers_[use.index].pending_incoherent_write : textures_[use.index].pending_incoherent_write) = true;
        }
    }
}

void RenderGraph::execute() {
    if (!compiled_) {
        compile();
    }
    PassContext context(*this);
    for (std::size_t p = 0; p < passes_.size(); ++p) {
        Pass& pass = *passes_[p];
        if (pass.culled) {
            continue;
        }
        for (std::uint32_t index : acquire_before_[p]) {
            TextureResource& r = textures_[index];
            r.physical = acquire_texture(r.desc, r.name);
        }
        for (std::uint32_t index : acquire_buffers_before_[p]) {
            BufferResource& r = buffers_[index];
            r.physical = acquire_buffer(r.size, r.name);
        }
        emit_barriers(pass);
        {
            gfx::ScopedPass scope(profiler_, pass.name.c_str());
            pass.run(context);
        }
        for (std::uint32_t index : release_after_[p]) {
            const TextureResource& r = textures_[index];
            if (r.physical >= 0) {
                pool_[static_cast<std::size_t>(r.physical)].in_use = false;
            }
        }
        for (std::uint32_t index : release_buffers_after_[p]) {
            const BufferResource& r = buffers_[index];
            if (r.physical >= 0) {
                buffer_pool_[static_cast<std::size_t>(r.physical)].in_use = false;
            }
        }
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

std::vector<PassInfo> RenderGraph::pass_info() const {
    std::vector<PassInfo> info;
    info.reserve(passes_.size());
    for (const auto& pass : passes_) {
        PassInfo entry;
        entry.name = pass->name;
        entry.culled = pass->culled;
        for (const Pass::Use& use : pass->uses) {
            const std::string& name = use.buffer ? buffers_[use.index].name : textures_[use.index].name;
            auto& list = use.write ? entry.writes : entry.reads;
            if (std::find(list.begin(), list.end(), name) == list.end()) {
                list.push_back(name);
            }
        }
        for (const auto& [name, ms] : last_timings_) {
            if (name == entry.name) {
                entry.gpu_ms = ms;
                break;
            }
        }
        info.push_back(std::move(entry));
    }
    return info;
}

std::vector<ResourceInfo> RenderGraph::resource_info() const {
    std::vector<ResourceInfo> info;
    for (const TextureResource& r : textures_) {
        ResourceInfo entry;
        entry.name = r.name;
        entry.desc = r.desc;
        entry.imported = r.imported != nullptr;
        entry.physical = r.physical;
        entry.first_pass = r.first;
        entry.last_pass = r.last;
        info.push_back(std::move(entry));
    }
    for (const BufferResource& r : buffers_) {
        ResourceInfo entry;
        entry.name = r.name;
        entry.buffer = true;
        entry.buffer_size = r.size;
        entry.imported = r.imported != nullptr;
        entry.physical = r.physical;
        entry.first_pass = r.first;
        entry.last_pass = r.last;
        info.push_back(std::move(entry));
    }
    return info;
}

std::size_t RenderGraph::pooled_texture_bytes() const {
    std::size_t bytes = 0;
    for (const PooledTexture& pooled : pool_) {
        const gfx::TextureDesc& d = pooled.texture.desc();
        std::size_t layers = d.target == GL_TEXTURE_CUBE_MAP ? 6 : d.target == GL_TEXTURE_CUBE_MAP_ARRAY ? static_cast<std::size_t>(d.depth) * 6 : static_cast<std::size_t>(d.depth);
        std::size_t level_bytes = static_cast<std::size_t>(d.width) * static_cast<std::size_t>(d.height) * layers * gfx::bytes_per_texel(d.format);
        bytes += d.mips > 1 ? level_bytes * 4 / 3 : level_bytes;
    }
    return bytes;
}

void RenderGraph::trim(int max_idle_frames) {
    std::vector<GLuint> removed;
    for (std::size_t i = 0; i < pool_.size();) {
        if (!pool_[i].in_use && frame_ - pool_[i].last_used > static_cast<std::uint64_t>(max_idle_frames)) {
            removed.push_back(pool_[i].texture.id());
            pool_.erase(pool_.begin() + static_cast<std::ptrdiff_t>(i));
        } else {
            ++i;
        }
    }
    for (GLuint id : removed) {
        framebuffers_.forget_texture(id);
    }
    for (std::size_t i = 0; i < buffer_pool_.size();) {
        if (!buffer_pool_[i].in_use && frame_ - buffer_pool_[i].last_used > static_cast<std::uint64_t>(max_idle_frames)) {
            buffer_pool_.erase(buffer_pool_.begin() + static_cast<std::ptrdiff_t>(i));
        } else {
            ++i;
        }
    }
}

void RenderGraph::clear_pool() {
    for (const PooledTexture& pooled : pool_) {
        framebuffers_.forget_texture(pooled.texture.id());
    }
    pool_.clear();
    buffer_pool_.clear();
}

} // namespace Hollow::rg

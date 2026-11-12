// skate1 - native D3D layer: renderer (see docs/native_d3d.md)
//
// Turns the game's D3D calls into plume work: draws (XenosRecomp shaders,
// state from the D3D device shadow, vertex/index buffers and textures from
// guest memory), clears and resolves. EDRAM render targets become host
// surfaces keyed by EDRAM base, format and pitch; resolves copy them into
// host textures keyed by destination address, which later texture fetches
// of that address sample instead of guest memory. Fed by the in-game hooks or
// by tools/native_replay from a frame capture.

#pragma once

#include <array>
#include <atomic>
#include <utility>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <plume_render_interface.h>

#include "xenos_decode.h"

struct ShaderCacheEntry;

namespace native_gpu {

// DrawIndexedVertices / DrawVertices / EndVertices as the device saw them.
struct DrawCall {
  xenos::DeviceState state;
  uint64_t vs_key = 0;
  uint64_t ps_key = 0;
  uint32_t primitive = 0;  // Xenos primitive type
  uint32_t base_vertex = 0;
  uint32_t start = 0;      // first index or vertex
  uint32_t count = 0;
  // The vertex shader's physical part as D3D patched it for this draw.
  const uint8_t* vs_physical = nullptr;
  uint32_t vs_physical_size = 0;
  // Indexed draws: the guest index buffer (physical address, size in bytes).
  uint32_t indices_address = 0;
  uint32_t indices_size = 0;
  bool indices_32bit = false;
  // BeginVertices / EndVertices: the vertices written into the command buffer.
  const uint8_t* inline_vertices = nullptr;
  uint32_t inline_size = 0;
};

// ClearF(flags, rect, color): flags 0xF color targets, 0x10 depth, 0x20 stencil.
struct ClearCall {
  xenos::DeviceState state;
  uint32_t flags = 0;
  bool has_rect = false;
  int32_t rect[4] = {};  // x1, y1, x2, y2
  float color[4] = {};
  float depth = 1.0f;    // the Z argument (f1)
  uint32_t stencil = 0;  // the stencil argument
};

// Resolve(flags, source rect, texture, dest point, ...): flags & 4 = depth,
// 0x100 / 0x200 clear color / depth afterwards. The destination comes from
// the RB_COPY_* registers.
struct ResolveCall {
  xenos::DeviceState state;
  uint32_t flags = 0;
  bool has_rect = false;
  int32_t rect[4] = {};
  bool has_dest_point = false;
  int32_t dest_x = 0;
  int32_t dest_y = 0;
  float clear_depth = 1.0f;  // ClearZ (f1), for flags 0x200
};

// Tells the renderer when the game writes guest memory it has converted
// (textures, vertex and index buffers), so it converts them again. Physical
// addresses.
class WriteWatcher {
 public:
  virtual ~WriteWatcher() = default;
  // Watches the range for writes, before its contents are read; returns the
  // stamp to pass to Dirty. Ranges may overlap, each has its own stamp.
  virtual uint32_t Watch(uint32_t address, uint32_t size) = 0;
  // Whether the range was written to since the Watch that returned `stamp`.
  virtual bool Dirty(uint32_t address, uint32_t size, uint32_t stamp) = 0;
};

class Renderer {
 public:
  // `physical` is the guest physical memory; `container` returns the shader
  // container (big-endian) for a cache key, for the vertex elements;
  // `watcher` may be null (guest memory never changes, as in a replay).
  bool Initialize(plume::RenderDevice* device, const uint8_t* physical,
                  std::function<const uint8_t*(uint64_t key)> container,
                  WriteWatcher* watcher = nullptr);

  void BeginFrame();
  void Draw(const DrawCall& call);
  void Clear(const ClearCall& call);
  void Resolve(const ResolveCall& call);
  // Submits the frame and waits for it. With a front buffer address, also
  // packs that resolved texture as A2B10G10R10 for presented().
  void EndFrame(uint32_t present_address = 0);

  // The packed front buffer of the frame before the last EndFrame's (frames
  // are pipelined; row pitch = width * 4), or null.
  const uint32_t* presented() const {
    return presented_ && presented_->present_ready ? presented_->present_pixels : nullptr;
  }
  // Scene surfaces are rendered and displayed this many times larger.
  // Takes effect at the next frame; smaller passes (shadows) keep their size.
  void SetRenderScale(float scale) { requested_scale_ = scale; }
  // The display gamma ramp to apply to presented frames (256 packed entries).
  void SetGammaRamp(const uint32_t* entries);
  // Scene surfaces this many times wider than 16:9; the interface stays centered.
  void SetWidth(float wide) { requested_wide_ = wide; }
  // Compiled pipelines kept between sessions: a first visit to a zone then costs
  // no compilation. Saving needs no pause and only happens after new pipelines.
  void LoadPipelineCache(const std::string& path);
  void SavePipelineCache();
  // The frame EndFrame just submitted, to read as soon as the GPU finishes it.
  // Its pixels stay valid until the next EndFrame, which the caller must not
  // enter before WaitSubmitted has returned.
  struct Submitted {
    const void* fence = nullptr;
    const uint32_t* pixels = nullptr;
    uint32_t width = 0, height = 0;
  };
  Submitted submitted() const;
  void WaitSubmitted(const Submitted& submitted);
  uint32_t presented_width() const { return presented_ ? presented_->present_width : 0; }
  uint32_t presented_height() const { return presented_ ? presented_->present_height : 0; }

  // Waits for all submitted frames.
  void WaitIdle();

  // Reads back a resolved texture (RGBA8, or R16G16 as 4 bytes per texel) by
  // its guest address.
  bool ReadResolved(uint32_t address, std::vector<uint8_t>& rgba, uint32_t& width,
                    uint32_t& height);

  // Why draws or textures were skipped, and counts, for reports.
  const std::map<std::string, uint32_t>& stats() const { return stats_; }
  // Where the renderer thread's time goes, for the pacing log.
  struct Timings {
    double gpu_wait_ms = 0.0, texture_ms = 0.0, buffer_ms = 0.0, submit_ms = 0.0, worst_texture_frame_ms = 0.0,
           worst_buffer_frame_ms = 0.0, worst_submit_ms = 0.0, texture_watch_ms = 0.0, texture_create_ms = 0.0;
    uint32_t textures = 0, buffers = 0, textures_unchanged = 0, textures_deferred = 0;
  };
  Timings TakeTimings() { return std::exchange(timings_, {}); }
  // Pipelines compiled since the last call, and the time spent (hitches).
  std::pair<uint32_t, double> TakePipelineStats() {
    return {pipelines_created_.exchange(0), pipeline_ms_.exchange(0.0)};
  }

 private:
  struct Shader {
    const ShaderCacheEntry* entry = nullptr;
    uint32_t microcode_offset = 0;
    struct Element {
      uint32_t address, usage, usage_index;
      uint32_t dst_register;  // the original vfetch's destination register
      uint32_t dst_swizzle;   // and its destination swizzle (components written)
    };
    std::vector<Element> elements;
    std::unique_ptr<plume::RenderShader> shader;
  };
  struct Texture {
    float scale = 1.0f;  // host pixels per guest pixel, vertically
    float scale_x = 1.0f;  // and horizontally
    std::unique_ptr<plume::RenderTexture> texture;
    std::unique_ptr<plume::RenderTextureView> view;
    plume::RenderFormat format = plume::RenderFormat::UNKNOWN;
    uint32_t width = 0, height = 0;
    uint32_t dimension = 1;  // 1 2D, 2 3D, 3 cube
    uint32_t index = 0;      // in its descriptor heap
    // Depth surfaces: a view of the stencil aspect, for depth resolves.
    std::unique_ptr<plume::RenderTextureView> stencil_view;
    uint32_t stencil_index = 0;
    // Resolved textures: views with the swizzles fetches asked for, and
    // their heap indices.
    std::unordered_map<uint32_t, std::pair<std::unique_ptr<plume::RenderTextureView>, uint32_t>>
        swizzled_views;
    uint32_t resolved_size = 0;  // the guest memory a resolve writes
    uint64_t content_hash = 0;   // guest textures, NATIVE_GPU_VERIFY_TEXTURES
    uint32_t watch_stamp = 0;    // guest and resolved textures: WriteWatcher::Watch's
  };
  uint32_t ResolvedIndex(Texture& texture, uint32_t swizzle);
  // Releases a resolved texture once the recording frame has completed.
  void RetireResolved(Texture& texture);

  Shader* GetShader(uint64_t key);
  Texture* GetGuestTexture(const xenos::TextureFetch& fetch);
  // A guest vertex or index buffer converted to host order by `convert`.
  const plume::RenderBuffer* GetGuestBuffer(uint32_t address, uint32_t size, uint64_t layout,
                                            const std::function<void(uint8_t*)>& convert);
  uint32_t GetSampler(const xenos::TextureFetch& fetch);
  float SurfaceScale(uint32_t pitch) const;
  float SurfaceWide(uint32_t pitch) const;
  Texture& GetColorSurface(const xenos::DeviceState& state);
  Texture& GetDepthSurface(const xenos::DeviceState& state);
  Texture& CreateSurface(uint64_t key, uint32_t width, uint32_t height, plume::RenderFormat format,
                         bool depth);
  void Transition(plume::RenderTexture* texture, plume::RenderTextureLayout layout);
  void SetTargets(plume::RenderTexture* color, plume::RenderTexture* depth);
  void SetMainLayout();
  // Records which EDRAM tiles the pass's targets write (`height` rows from the
  // top). With `check`, first clears the color target's tiles that another
  // surface wrote since it did.
  void TrackEdram(const xenos::DeviceState& state, Texture& color, Texture& depth, bool color_written,
                  bool depth_written, uint32_t height, bool check);
  uint64_t Allocate(const void* data, uint64_t size, uint64_t alignment = 256);

  struct GuestBuffer {
    std::unique_ptr<plume::RenderBuffer> buffer;
    uint64_t content_hash = 0;  // NATIVE_GPU_VERIFY_TEXTURES
    uint32_t watch_stamp = 0;
  };

  plume::RenderDevice* device_ = nullptr;
  WriteWatcher* watcher_ = nullptr;
  std::unordered_map<uint64_t, GuestBuffer> guest_buffers_;
  std::vector<std::unique_ptr<plume::RenderBuffer>> retired_buffers_;
  const uint8_t* physical_ = nullptr;
  std::function<const uint8_t*(uint64_t)> container_;
  std::vector<uint8_t> spirv_;

  // Frames in flight: the game's render thread records frame N while the GPU
  // runs frame N-1. Each slot owns what its commands use.
  struct Frame {
    std::unique_ptr<plume::RenderCommandList> commands;
    std::unique_ptr<plume::RenderCommandFence> fence;
    std::unique_ptr<plume::RenderBuffer> upload;  // per-frame uploads
    bool pending = false;
    // Released once the frame has completed.
    std::vector<std::unique_ptr<plume::RenderBuffer>> retired_buffers;
    std::vector<std::unique_ptr<plume::RenderTexture>> retired_textures;
    std::vector<std::unique_ptr<plume::RenderTextureView>> retired_views;
    std::vector<std::unique_ptr<plume::RenderFramebuffer>> retired_framebuffers;
    std::vector<std::pair<uint32_t, uint32_t>> retired_slots;  // heap (dimension), index
    // The packed front buffer read back by this frame.
    std::unique_ptr<plume::RenderBuffer> present_readback;
    const uint32_t* present_pixels = nullptr;
    uint32_t present_width = 0, present_height = 0;
    bool present_ready = false;
  };
  static constexpr uint32_t kFramesInFlight = 2;
  void WaitFrame(Frame& frame);
  void UseUploadBuffer(plume::RenderBuffer* buffer, uint64_t size);

  std::unique_ptr<plume::RenderCommandQueue> queue_;
  Frame frames_[kFramesInFlight];
  uint32_t frame_ = 0;
  uint64_t frame_number_ = 0;
  const Frame* presented_ = nullptr;
  plume::RenderCommandList* commands_ = nullptr;  // the recording frame's
  std::unique_ptr<plume::RenderDescriptorSet> texture_2d_set_, texture_3d_set_, texture_cube_set_;
  std::unique_ptr<plume::RenderDescriptorSet> sampler_set_;
  std::unique_ptr<plume::RenderPipelineLayout> layout_, resolve_layout_;
  std::unique_ptr<plume::RenderShader> resolve_vs_, resolve_ps_, pack_ps_;
  std::unique_ptr<plume::RenderTexture> present_texture_;
  uint32_t present_width_ = 0, present_height_ = 0;
  // The recording frame's upload buffer (its slot's, or an overflow one).
  plume::RenderBuffer* upload_ = nullptr;
  uint8_t* upload_memory_ = nullptr;
  uint64_t upload_address_ = 0, upload_offset_ = 0, upload_size_ = 0;
  static constexpr uint64_t kUploadSize = 64ull << 20;

  std::unique_ptr<plume::RenderTexture> null_2d_, null_3d_, null_cube_;
  std::unique_ptr<plume::RenderTextureView> null_cube_view_;
  uint32_t next_2d_ = 1, next_3d_ = 1, next_cube_ = 1;
  // Descriptor slots by heap (Texture::dimension - 1). A replaced texture gets
  // a new one: commands already recorded, and the frame on the GPU, keep
  // reading the old texture through the old slot until they have completed.
  uint32_t AllocateSlot(uint32_t dimension);
  std::vector<uint32_t> free_slots_[3];
  std::vector<std::pair<uint32_t, uint32_t>> retired_slots_;

  std::unordered_map<uint64_t, Shader> shaders_;
  std::unordered_map<uint64_t, std::unique_ptr<plume::RenderPipeline>> pipelines_;
  std::unordered_map<uint64_t, Texture> guest_textures_;
  std::unordered_map<uint64_t, uint32_t> sampler_indices_;
  std::vector<std::unique_ptr<plume::RenderSampler>> samplers_;
  std::unordered_map<uint64_t, Texture> surfaces_;   // EDRAM render targets
  std::unordered_map<uint32_t, Texture> resolved_;   // by destination address
  std::map<std::pair<plume::RenderTexture*, plume::RenderTexture*>,
           std::unique_ptr<plume::RenderFramebuffer>> framebuffers_;
  std::unordered_map<plume::RenderTexture*, plume::RenderTextureLayout> layouts_;
  // Textures replaced during the frame, released once it has completed.
  std::vector<std::unique_ptr<plume::RenderTexture>> retired_textures_;
  std::vector<std::unique_ptr<plume::RenderTextureView>> retired_views_;
  std::vector<std::unique_ptr<plume::RenderFramebuffer>> retired_framebuffers_;
  std::pair<plume::RenderTexture*, plume::RenderTexture*> current_targets_{};
  // The surface that last wrote each EDRAM tile (5120 bytes). Surfaces at
  // overlapping EDRAM ranges share memory on the GPU but not here.
  std::vector<const Texture*> edram_owners_ = std::vector<const Texture*>(2048);
  bool main_layout_bound_ = false;
  std::map<std::string, uint32_t> stats_;
  uint64_t TextureContentHash(const xenos::TextureFetch& t, uint32_t byte_size) const;
  std::vector<uint8_t> convert_buffer_;
  float requested_wide_ = 1.0f, render_wide_ = 1.0f;
  uint32_t world_draws_ = 0, world_draws_last_frame_ = 0;
  bool interface_cover_ = false;  // a full-screen interface quad this frame
  std::string pipeline_cache_path_;
  uint32_t unsaved_pipelines_ = 0;
  std::atomic<bool> pipeline_cache_saving_{false};
  std::array<uint32_t, 256> gamma_ramp_{};
  bool gamma_ramp_dirty_ = false;
  std::unique_ptr<plume::RenderTexture> gamma_texture_;
  std::unique_ptr<plume::RenderTextureView> gamma_view_;
  uint32_t gamma_index_ = 0;
  float requested_scale_ = 1.0f, render_scale_ = 1.0f;
  std::vector<plume::RenderVertexBufferView> vertex_views_;
  Texture* last_color_ = nullptr;
  Texture* last_depth_ = nullptr;
  uint64_t last_color_key_ = 0, last_depth_key_ = 0;
  // Per-draw work done once. Each compares what it depends on with the last draw.
  struct VertexLayout {
    bool valid = false, packed_normals = false, half_normals = false;
    std::vector<plume::RenderInputElement> elements;
    std::vector<plume::RenderInputSlot> slots;
    std::vector<uint32_t> slot_fetch;
    std::vector<std::vector<std::pair<uint32_t, uint32_t>>> half_swaps, narrowed, packed;
    std::vector<uint64_t> slot_layouts;
  };
  std::unordered_map<uint64_t, VertexLayout> vertex_layouts_;
  struct ConstantUpload {
    const void* upload = nullptr;
    uint64_t offset = 0;
    std::array<uint8_t, 4096> source{};
  };
  ConstantUpload vs_constants_, ps_constants_;
  struct EdramUse {
    const void* color = nullptr;
    const void* depth = nullptr;
    uint32_t surface_info = 0, color_info = 0, depth_info = 0, height = 0;
    uint32_t color_written = 0, depth_written = 0;
  };
  EdramUse last_edram_use_;
  struct SlotBinding {
    uint32_t generation = 0;
    uint8_t fetch[24] = {};
    uint32_t texture_2d = 0, texture_3d = 0, texture_cube = 0, sampler = 0;
  };
  std::array<SlotBinding, 1024> bindings_;
  uint32_t texture_generation_ = 1;
  const void* bound_pipeline_ = nullptr;
  float bound_viewport_[8] = {}, bound_bias_[2] = {};
  bool has_bound_viewport_ = false, has_bound_bias_ = false;
  Timings timings_;
  double texture_frame_ms_ = 0.0, buffer_frame_ms_ = 0.0;
  std::atomic<uint32_t> pipelines_created_{0};
  std::atomic<double> pipeline_ms_{0.0};
};

}  // namespace native_gpu

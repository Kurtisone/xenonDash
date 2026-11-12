// SPDX-License-Identifier: GPL-3.0-or-later
// ngpu_core - noyau de rendu natif Vulkan commun (XenonDash, P8).
// Reprise de Skate1Native (Kurtisone/Skate1Recompiled, GPL-3.0), src/native_gpu/renderer.cpp, commit 81805f51 ;
// ce moteur reprend lui-meme les structures du renderer d'Unleashed Recompiled (hedge-dev, GPL-3.0, tas bindless,
// constantes partagees XenosRecomp). Texte d'origine : "skate1 - native D3D layer: renderer".
// Modifications par rapport a Skate1Native (voir docs/ngpu_core.md) : cache de shaders via ShaderProvider au lieu
// des tables compilees, listes de shaders d'interface et plage de pitch de la scene dans un Profile, taille du
// tampon d'indices/sommets en ligne (UP) ; aucune dependance au jeu ni au SDK.

#include "renderer.h"

#include <string_view>
#include <cfloat>
#include <thread>
#include <chrono>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#define XXH_INLINE_ALL
#include <xxhash.h>

#include <plume_vulkan.h>

#include "shaders/native_shaders.h"

using namespace plume;

namespace native_gpu {

namespace {

// Bindless heap sizes.
constexpr uint32_t kTextureHeapSize = 8192;
constexpr uint32_t kSamplerHeapSize = 256;
// Largest EDRAM surface height: 10 MB of EDRAM.
constexpr uint32_t kMaxSurfaceHeight = 2048;

// XenosRecomp's shared constants (SharedConstants in Unleashed's video.cpp).
struct SharedConstants {
  uint32_t texture2DIndices[16]{};
  uint32_t texture3DIndices[16]{};
  uint32_t textureCubeIndices[16]{};
  uint32_t samplerIndices[16]{};
  uint32_t booleans{};
  uint32_t swappedTexcoords{};
  float halfPixelOffsetX{};
  float halfPixelOffsetY{};
  float alphaThreshold{};
};

struct ResolvePushConstants {
  uint32_t source_index;
  uint32_t mode;  // 0 color * scale, 1 depth packed as D24S8 in 8_8_8_8
  float scale;
  int32_t offset_x;
  int32_t offset_y;
  uint32_t stencil_index;
};

RenderStencilFaceDesc StencilFace(uint32_t func, uint32_t fail, uint32_t zpass, uint32_t zfail) {
  RenderStencilFaceDesc face;
  face.compareFunction = RenderComparisonFunction(func + 1);
  face.failOp = RenderStencilOp(fail + 1);
  face.passOp = RenderStencilOp(zpass + 1);
  face.depthFailOp = RenderStencilOp(zfail + 1);
  return face;
}

// XenosRecomp's USAGE_LOCATIONS: vertex input location of (usage, index).
int32_t UsageLocation(uint32_t usage, uint32_t index) {
  static constexpr struct {
    uint32_t usage, index, location;
  } kLocations[] = {
      {0, 0, 0},  {3, 0, 1},  {6, 0, 2},  {7, 0, 3},  {5, 0, 4},  {5, 1, 5},
      {5, 2, 6},  {5, 3, 7},  {10, 0, 8}, {2, 0, 9},  {1, 0, 10}, {10, 1, 11},
      {5, 4, 12}, {5, 5, 13}, {5, 6, 14}, {5, 7, 15}, {0, 1, 15},
  };
  for (const auto& entry : kLocations) {
    if (entry.usage == usage && entry.index == index) return int32_t(entry.location);
  }
  return -1;
}

// XenosRecomp declares normals, tangents and binormals uint4 and decodes them
// itself: packed 10_11_11 (SPEC_CONSTANT_R11G11B10_NORMAL) or float bits.
bool IsRawUsage(uint32_t usage) { return usage == 3 || usage == 6 || usage == 7; }

RenderFormat RawVertexFormat(uint32_t format) {
  using namespace xenos;
  switch (format) {
    case k_10_11_11: return RenderFormat::R32_UINT;
    case k_8_8_8_8: return RenderFormat::R32_UINT;  // packed to 10_11_11 (see Draw)
    case k_32_FLOAT: return RenderFormat::R32_UINT;
    case k_32_32_FLOAT: return RenderFormat::R32G32_UINT;
    case k_32_32_32_FLOAT: return RenderFormat::R32G32B32_UINT;
    case k_32_32_32_32_FLOAT: return RenderFormat::R32G32B32A32_UINT;
    // Half floats, converted by XenosRecomp with SPEC_CONSTANT_HALF_NORMAL.
    case k_16_16_FLOAT: return RenderFormat::R16G16_UINT;
    case k_16_16_16_16_FLOAT: return RenderFormat::R16G16B16A16_UINT;
    default: return RenderFormat::UNKNOWN;
  }
}

RenderFormat VertexFormat(const xenos::VertexFetchInstruction& fetch, uint32_t usage) {
  using namespace xenos;
  const bool s = fetch.is_signed, i = fetch.is_integer;
  switch (fetch.format) {
    case k_8_8_8_8:
      if (usage == 2) {  // BLENDINDICES: uint4
        return s ? RenderFormat::R8G8B8A8_SINT : RenderFormat::R8G8B8A8_UINT;
      }
      // Integer components feed float4 inputs as their values (scaled).
      return i ? (s ? RenderFormat::R8G8B8A8_SSCALED : RenderFormat::R8G8B8A8_USCALED)
               : (s ? RenderFormat::R8G8B8A8_SNORM : RenderFormat::R8G8B8A8_UNORM);
    case k_16_16:
      return i ? (s ? RenderFormat::R16G16_SSCALED : RenderFormat::R16G16_USCALED)
               : (s ? RenderFormat::R16G16_SNORM : RenderFormat::R16G16_UNORM);
    case k_16_16_16_16:
      return i ? (s ? RenderFormat::R16G16B16A16_SSCALED : RenderFormat::R16G16B16A16_USCALED)
               : (s ? RenderFormat::R16G16B16A16_SNORM : RenderFormat::R16G16B16A16_UNORM);
    case k_10_11_11:  // no such host format: the buffer is narrowed to 8 bits (see Draw)
      return s ? RenderFormat::R8G8B8A8_SNORM : RenderFormat::R8G8B8A8_UNORM;
    case k_16_16_FLOAT: return RenderFormat::R16G16_FLOAT;
    case k_16_16_16_16_FLOAT: return RenderFormat::R16G16B16A16_FLOAT;
    case k_32: return s ? RenderFormat::R32_SINT : RenderFormat::R32_UINT;
    case k_32_32: return s ? RenderFormat::R32G32_SINT : RenderFormat::R32G32_UINT;
    case k_32_32_32_32: return s ? RenderFormat::R32G32B32A32_SINT : RenderFormat::R32G32B32A32_UINT;
    case k_32_FLOAT: return RenderFormat::R32_FLOAT;
    case k_32_32_FLOAT: return RenderFormat::R32G32_FLOAT;
    case k_32_32_32_32_FLOAT: return RenderFormat::R32G32B32A32_FLOAT;
    case k_32_32_32_FLOAT: return RenderFormat::R32G32B32_FLOAT;
    default: return RenderFormat::UNKNOWN;
  }
}

RenderFormat TextureFormat(const xenos::TextureFetch& t) {
  using namespace xenos;
  const bool s = t.is_signed;
  switch (t.format) {
    case kTex_8: return s ? RenderFormat::R8_SNORM : RenderFormat::R8_UNORM;
    case kTex_8_8: return s ? RenderFormat::R8G8_SNORM : RenderFormat::R8G8_UNORM;
    case kTex_8_8_8_8: return s ? RenderFormat::R8G8B8A8_SNORM : RenderFormat::R8G8B8A8_UNORM;
    case kTex_DXT1: return RenderFormat::BC1_UNORM;
    case kTex_DXT2_3: return RenderFormat::BC2_UNORM;
    case kTex_DXT4_5: return RenderFormat::BC3_UNORM;
    case kTex_16_16: return s ? RenderFormat::R16G16_SNORM : RenderFormat::R16G16_UNORM;
    case kTex_16_16_16_16: return s ? RenderFormat::R16G16B16A16_SNORM : RenderFormat::R16G16B16A16_UNORM;
    case kTex_16_16_FLOAT: return RenderFormat::R16G16_FLOAT;
    case kTex_16_16_16_16_FLOAT: return RenderFormat::R16G16B16A16_FLOAT;
    case kTex_32_FLOAT: return RenderFormat::R32_FLOAT;
    default: return RenderFormat::UNKNOWN;
  }
}

// EDRAM color formats (RB_COLOR_INFO) as host render target formats. 16_16
// and 16_16_16_16 hold -32..32 in EDRAM, kept as floats here.
RenderFormat SurfaceFormat(uint32_t color_format) {
  switch (color_format) {
    case 0: case 1: return RenderFormat::R8G8B8A8_UNORM;  // 8_8_8_8 (_GAMMA)
    case 4: case 6: return RenderFormat::R16G16_FLOAT;
    case 5: case 7: case 2: case 3: case 10: case 12: return RenderFormat::R16G16B16A16_FLOAT;
    case 14: return RenderFormat::R32_FLOAT;
    case 15: return RenderFormat::R32G32_FLOAT;
    default: return RenderFormat::R8G8B8A8_UNORM;
  }
}

// The fetch constant's swizzle (3 bits per destination component: 0-3 source
// component, 4 zero, 5 one) as a view component mapping.
RenderComponentMapping SwizzleMapping(uint32_t swizzle) {
  RenderSwizzle components[4];
  for (uint32_t c = 0; c < 4; ++c) {
    const uint32_t select = (swizzle >> (3 * c)) & 7;
    components[c] = select < 4 ? RenderSwizzle(uint32_t(RenderSwizzle::R) + select)
                               : select == 4 ? RenderSwizzle::ZERO : RenderSwizzle::ONE;
  }
  return RenderComponentMapping(components[0], components[1], components[2], components[3]);
}

RenderBlend BlendFactor(uint32_t factor) {
  switch (factor) {
    case 0: return RenderBlend::ZERO;
    case 1: return RenderBlend::ONE;
    case 4: return RenderBlend::SRC_COLOR;
    case 5: return RenderBlend::INV_SRC_COLOR;
    case 6: return RenderBlend::SRC_ALPHA;
    case 7: return RenderBlend::INV_SRC_ALPHA;
    case 8: return RenderBlend::DEST_COLOR;
    case 9: return RenderBlend::INV_DEST_COLOR;
    case 10: return RenderBlend::DEST_ALPHA;
    case 11: return RenderBlend::INV_DEST_ALPHA;
    case 12: case 14: return RenderBlend::BLEND_FACTOR;
    case 13: case 15: return RenderBlend::INV_BLEND_FACTOR;
    case 16: return RenderBlend::SRC_ALPHA_SAT;
    default: return RenderBlend::ONE;
  }
}

RenderBlendOperation BlendOp(uint32_t op) {
  switch (op) {
    case 1: return RenderBlendOperation::SUBTRACT;
    case 2: return RenderBlendOperation::MIN;
    case 3: return RenderBlendOperation::MAX;
    case 4: return RenderBlendOperation::REV_SUBTRACT;
    default: return RenderBlendOperation::ADD;
  }
}

void CopySwapped32(uint8_t* dst, const uint8_t* src, size_t bytes) {
  for (size_t i = 0; i < bytes; i += 4) {
    const uint32_t value = xenos::LoadBE32(src + i);
    std::memcpy(dst + i, &value, 4);
  }
}

}  // namespace

bool Renderer::Initialize(RenderDevice* device, const uint8_t* physical,
                          std::function<const uint8_t*(uint64_t key)> container, ShaderProvider* shaders,
                          WriteWatcher* watcher, const Profile& profile) {
  device_ = device;
  watcher_ = watcher;
  physical_ = physical;
  container_ = std::move(container);
  shader_provider_ = shaders;
  profile_ = profile;
  if (!shader_provider_) return false;
  queue_ = device_->createCommandQueue(RenderCommandListType::DIRECT);
  for (Frame& frame : frames_) {
    frame.commands = queue_->createCommandList();
    frame.fence = device_->createCommandFence();
    frame.upload = device_->createBuffer(RenderBufferDesc::UploadBuffer(
        kUploadSize, RenderBufferFlag::CONSTANT | RenderBufferFlag::VERTEX | RenderBufferFlag::INDEX));
  }
  commands_ = frames_[0].commands.get();
  UseUploadBuffer(frames_[0].upload.get(), kUploadSize);

  // Bindless heaps and pipeline layout as Unleashed sets them up: 2D, 3D and
  // cube texture heaps, a sampler heap, and 24 bytes of push constants (the
  // vertex, pixel and shared constant buffer addresses).
  RenderDescriptorSetBuilder textures;
  textures.begin();
  textures.addTexture(0, kTextureHeapSize);
  textures.end(true, kTextureHeapSize);
  RenderDescriptorSetBuilder samplers;
  samplers.begin();
  samplers.addSampler(0, kSamplerHeapSize);
  samplers.end(true, kSamplerHeapSize);
  texture_2d_set_ = textures.create(device_);
  texture_3d_set_ = textures.create(device_);
  texture_cube_set_ = textures.create(device_);
  sampler_set_ = samplers.create(device_);
  RenderPipelineLayoutBuilder layout;
  layout.begin(false, true);
  layout.addDescriptorSet(textures);
  layout.addDescriptorSet(textures);
  layout.addDescriptorSet(textures);
  layout.addDescriptorSet(samplers);
  layout.addPushConstant(0, 4, 24, RenderShaderStageFlag::VERTEX | RenderShaderStageFlag::PIXEL);
  layout.end();
  layout_ = layout.create(device_);
  RenderPipelineLayoutBuilder resolveLayout;
  resolveLayout.begin(false, false);
  resolveLayout.addDescriptorSet(textures);
  resolveLayout.addPushConstant(0, 0, sizeof(ResolvePushConstants), RenderShaderStageFlag::PIXEL);
  resolveLayout.end();
  resolve_layout_ = resolveLayout.create(device_);
  resolve_vs_ = device_->createShader(k_resolve_vs, sizeof(k_resolve_vs), "VSMain", RenderShaderFormat::SPIRV);
  resolve_ps_ = device_->createShader(k_resolve_ps, sizeof(k_resolve_ps), "PSMain", RenderShaderFormat::SPIRV);
  pack_ps_ = device_->createShader(k_pack_ps, sizeof(k_pack_ps), "PSPack", RenderShaderFormat::SPIRV);


  // Index 0 of each heap: 1x1 white textures; sampler 0: linear wrap.
  null_2d_ = device_->createTexture(RenderTextureDesc::Texture2D(1, 1, 1, RenderFormat::R8G8B8A8_UNORM));
  null_3d_ = device_->createTexture(RenderTextureDesc::Texture3D(1, 1, 1, 1, RenderFormat::R8G8B8A8_UNORM));
  null_cube_ = device_->createTexture(RenderTextureDesc::Texture(
      RenderTextureDimension::TEXTURE_2D, 1, 1, 1, 1, 6, RenderFormat::R8G8B8A8_UNORM, RenderTextureFlag::CUBE));
  RenderTextureViewDesc cubeView;
  cubeView.format = RenderFormat::R8G8B8A8_UNORM;
  cubeView.dimension = RenderTextureViewDimension::TEXTURE_CUBE;
  cubeView.mipLevels = 1;
  null_cube_view_ = null_cube_->createTextureView(cubeView);
  samplers_.push_back(device_->createSampler(RenderSamplerDesc()));
  sampler_set_->setSampler(0, samplers_.back().get());

  commands_->begin();
  const uint32_t white = 0xFFFFFFFF;
  const uint64_t whiteOffset = Allocate(&white, 4, 512);
  for (RenderTexture* texture : {null_2d_.get(), null_3d_.get(), null_cube_.get()}) {
    Transition(texture, RenderTextureLayout::COPY_DEST);
    for (uint32_t slice = 0; slice < (texture == null_cube_.get() ? 6u : 1u); ++slice) {
      commands_->copyTextureRegion(
          RenderTextureCopyLocation::Subresource(texture, 0, slice),
          RenderTextureCopyLocation::PlacedFootprint(upload_, RenderFormat::R8G8B8A8_UNORM, 1, 1, 1,
                                                     64, whiteOffset));
    }
    Transition(texture, RenderTextureLayout::SHADER_READ);
  }
  texture_2d_set_->setTexture(0, null_2d_.get(), RenderTextureLayout::SHADER_READ);
  texture_3d_set_->setTexture(0, null_3d_.get(), RenderTextureLayout::SHADER_READ);
  texture_cube_set_->setTexture(0, null_cube_.get(), RenderTextureLayout::SHADER_READ, null_cube_view_.get());
  commands_->end();
  const RenderCommandList* lists[] = {commands_};
  queue_->executeCommandLists(lists, 1, nullptr, 0, nullptr, 0, frames_[0].fence.get());
  queue_->waitForCommandFence(frames_[0].fence.get());
  upload_offset_ = 0;
  return true;
}

void Renderer::UseUploadBuffer(RenderBuffer* buffer, uint64_t size) {
  upload_ = buffer;
  upload_memory_ = static_cast<uint8_t*>(buffer->map());  // persistently mapped
  upload_address_ = buffer->getDeviceAddress();
  upload_size_ = size;
  upload_offset_ = 0;
}

void Renderer::WaitFrame(Frame& frame) {
  if (!frame.pending) return;
  const auto wait_start = std::chrono::steady_clock::now();
  queue_->waitForCommandFence(frame.fence.get());
  timings_.gpu_wait_ms +=
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - wait_start).count();
  frame.pending = false;
  frame.retired_framebuffers.clear();
  frame.retired_buffers.clear();
  frame.retired_views.clear();
  frame.retired_textures.clear();
  for (const auto& [dimension, index] : frame.retired_slots) free_slots_[dimension - 1].push_back(index);
  frame.retired_slots.clear();
}

uint32_t Renderer::AllocateSlot(uint32_t dimension) {
  std::vector<uint32_t>& free = free_slots_[dimension - 1];
  if (!free.empty()) {
    const uint32_t index = free.back();
    free.pop_back();
    return index;
  }
  uint32_t& next = dimension == 2 ? next_3d_ : dimension == 3 ? next_cube_ : next_2d_;
  if (next >= kTextureHeapSize) {
    ++stats_["texture heap full"];
    return 0;  // the null texture
  }
  return next++;
}

void Renderer::WaitIdle() {
  for (Frame& frame : frames_) WaitFrame(frame);
}

uint64_t Renderer::Allocate(const void* data, uint64_t size, uint64_t alignment) {
  upload_offset_ = (upload_offset_ + alignment - 1) & ~(alignment - 1);
  if (upload_offset_ + size > upload_size_) {
    // Full (texture streaming): continue in an overflow buffer that lives
    // until this frame completes.
    ++stats_["upload overflow buffers"];
    const uint64_t overflowSize = std::max<uint64_t>(kUploadSize, size + alignment);
    auto overflow = device_->createBuffer(RenderBufferDesc::UploadBuffer(
        overflowSize, RenderBufferFlag::CONSTANT | RenderBufferFlag::VERTEX | RenderBufferFlag::INDEX));
    UseUploadBuffer(overflow.get(), overflowSize);
    retired_buffers_.push_back(std::move(overflow));
  }
  const uint64_t at = upload_offset_;
  if (data) std::memcpy(upload_memory_ + at, data, size);
  upload_offset_ += size;
  return at;
}

void Renderer::Transition(RenderTexture* texture, RenderTextureLayout layout) {
  RenderTextureLayout& current = layouts_[texture];
  if (current == layout) return;
  current = layout;
  RenderTextureBarrier barrier(texture, layout);
  commands_->barriers(RenderBarrierStage::GRAPHICS | RenderBarrierStage::COPY, nullptr, 0, &barrier, 1);
}

void Renderer::SetTargets(RenderTexture* color, RenderTexture* depth) {
  Transition(color, RenderTextureLayout::COLOR_WRITE);
  if (depth) Transition(depth, RenderTextureLayout::DEPTH_WRITE);
  if (current_targets_ == std::make_pair(color, depth)) return;
  auto& framebuffer = framebuffers_[{color, depth}];
  if (!framebuffer) {
    const RenderTexture* attachments[] = {color};
    framebuffer = device_->createFramebuffer(RenderFramebufferDesc(attachments, 1, depth));
  }
  commands_->setFramebuffer(framebuffer.get());
  current_targets_ = {color, depth};
}

void Renderer::SetMainLayout() {
  if (main_layout_bound_) return;
  bound_pipeline_ = nullptr;  // other passes bind their own state
  has_bound_viewport_ = has_bound_bias_ = false;
  commands_->setGraphicsPipelineLayout(layout_.get());
  commands_->setGraphicsDescriptorSet(texture_2d_set_.get(), 0);
  commands_->setGraphicsDescriptorSet(texture_3d_set_.get(), 1);
  commands_->setGraphicsDescriptorSet(texture_cube_set_.get(), 2);
  commands_->setGraphicsDescriptorSet(sampler_set_.get(), 3);
  main_layout_bound_ = true;
}

void Renderer::BeginFrame() {
  ++frame_number_;
  ++texture_generation_;  // what the game wrote is looked at once a frame
  world_draws_last_frame_ = std::exchange(world_draws_, 0);
  interface_cover_ = false;
  if (render_scale_ != requested_scale_ || render_wide_ != requested_wide_) {
    render_scale_ = requested_scale_;
    render_wide_ = requested_wide_;
    last_color_ = last_depth_ = nullptr;
  }
  last_edram_use_ = {};
  Frame& frame = frames_[frame_];
  WaitFrame(frame);
  commands_ = frame.commands.get();
  UseUploadBuffer(frame.upload.get(), kUploadSize);
  commands_->begin();
  current_targets_ = {};
  main_layout_bound_ = false;
}

void Renderer::EndFrame(uint32_t present_address) {
  Frame& frame = frames_[frame_];
  frame.present_ready = false;
  auto front = resolved_.find(present_address & 0xFFFFF000);
  if (present_address && front != resolved_.end() && front->second.texture) {
    // Pack the front buffer as A2B10G10R10 and read it back.
    Texture& source = front->second;
    if (gamma_ramp_dirty_) {
      gamma_ramp_dirty_ = false;
      if (gamma_texture_) {
        layouts_.erase(gamma_texture_.get());
        retired_textures_.push_back(std::move(gamma_texture_));
        retired_views_.push_back(std::move(gamma_view_));
      }
      gamma_texture_ = device_->createTexture(RenderTextureDesc::Texture(
          RenderTextureDimension::TEXTURE_2D, 256, 1, 1, 1, 1, RenderFormat::R32_UINT, RenderTextureFlag::NONE));
      Transition(gamma_texture_.get(), RenderTextureLayout::COPY_DEST);
      const uint64_t at = Allocate(gamma_ramp_.data(), sizeof(gamma_ramp_), 512);
      commands_->copyTextureRegion(RenderTextureCopyLocation::Subresource(gamma_texture_.get(), 0, 0),
                                   RenderTextureCopyLocation::PlacedFootprint(upload_, RenderFormat::R32_UINT, 256, 1,
                                                                              1, 256, at));
      Transition(gamma_texture_.get(), RenderTextureLayout::SHADER_READ);
      RenderTextureViewDesc view;
      view.format = RenderFormat::R32_UINT;
      view.dimension = RenderTextureViewDimension::TEXTURE_2D;
      view.mipLevels = 1;
      gamma_view_ = gamma_texture_->createTextureView(view);
      if (!gamma_index_) gamma_index_ = AllocateSlot(1);
      texture_2d_set_->setTexture(gamma_index_, gamma_texture_.get(), RenderTextureLayout::SHADER_READ,
                                  gamma_view_.get());
    }
    if (!present_texture_ || present_width_ != source.width || present_height_ != source.height) {
      present_width_ = source.width;
      present_height_ = source.height;
      if (present_texture_) {
        for (auto it = framebuffers_.begin(); it != framebuffers_.end();) {
          if (it->first.first == present_texture_.get()) {
            retired_framebuffers_.push_back(std::move(it->second));
            it = framebuffers_.erase(it);
          } else {
            ++it;
          }
        }
        layouts_.erase(present_texture_.get());
        retired_textures_.push_back(std::move(present_texture_));
      }
      present_texture_ = device_->createTexture(
          RenderTextureDesc::ColorTarget(present_width_, present_height_, RenderFormat::R32_UINT));
    }
    if (frame.present_width != present_width_ || frame.present_height != present_height_) {
      if (frame.present_readback) frame.retired_buffers.push_back(std::move(frame.present_readback));
      frame.present_width = present_width_;
      frame.present_height = present_height_;
      frame.present_readback = device_->createBuffer(
          RenderBufferDesc::ReadbackBuffer(uint64_t(present_width_) * present_height_ * 4));
      frame.present_pixels = static_cast<const uint32_t*>(frame.present_readback->map());
    }
    Transition(source.texture.get(), RenderTextureLayout::SHADER_READ);
    SetTargets(present_texture_.get(), nullptr);
    auto& pipeline = pipelines_[uint64_t(RenderFormat::R32_UINT) | 1ull << 61];
    if (!pipeline) {
      RenderGraphicsPipelineDesc desc;
      desc.pipelineLayout = resolve_layout_.get();
      desc.vertexShader = resolve_vs_.get();
      desc.pixelShader = pack_ps_.get();
      desc.renderTargetFormat[0] = RenderFormat::R32_UINT;
      desc.renderTargetBlend[0] = RenderBlendDesc::Copy();
      desc.renderTargetCount = 1;
      pipeline = device_->createGraphicsPipeline(desc);
    }
    commands_->setGraphicsPipelineLayout(resolve_layout_.get());
    commands_->setGraphicsDescriptorSet(texture_2d_set_.get(), 0);
    main_layout_bound_ = false;
    commands_->setPipeline(pipeline.get());
    ResolvePushConstants push{source.index, 0, 1.0f, 0, 0, gamma_index_};
    commands_->setGraphicsPushConstants(0, &push);
    RenderViewport viewport(0.0f, 0.0f, float(present_width_), float(present_height_));
    RenderRect scissor(0, 0, int32_t(present_width_), int32_t(present_height_));
    commands_->setViewports(&viewport, 1);
    commands_->setScissors(&scissor, 1);
    commands_->drawInstanced(3, 1, 0, 0);
    Transition(present_texture_.get(), RenderTextureLayout::COPY_SOURCE);
    // plume has no texture-to-buffer copy.
    VkBufferImageCopy region{};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = {present_width_, present_height_, 1};
    vkCmdCopyImageToBuffer(static_cast<VulkanCommandList*>(commands_)->vk,
                           static_cast<VulkanTexture*>(present_texture_.get())->vk,
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           static_cast<VulkanBuffer*>(frame.present_readback.get())->vk, 1, &region);
    frame.present_ready = true;
  }
  commands_->end();
  const RenderCommandList* lists[] = {commands_};
  const auto submit_start = std::chrono::steady_clock::now();
  queue_->executeCommandLists(lists, 1, nullptr, 0, nullptr, 0, frame.fence.get());
  const double submit_ms =
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - submit_start).count();
  timings_.submit_ms += submit_ms;
  timings_.worst_submit_ms = std::max(timings_.worst_submit_ms, submit_ms);
  frame.pending = true;
  // What this frame's commands use is released once it has completed.
  for (auto& b : retired_buffers_) frame.retired_buffers.push_back(std::move(b));
  for (auto& t : retired_textures_) frame.retired_textures.push_back(std::move(t));
  for (auto& v : retired_views_) frame.retired_views.push_back(std::move(v));
  frame.retired_slots.insert(frame.retired_slots.end(), retired_slots_.begin(), retired_slots_.end());
  retired_slots_.clear();
  for (auto& f : retired_framebuffers_) frame.retired_framebuffers.push_back(std::move(f));
  retired_buffers_.clear();
  retired_textures_.clear();
  retired_views_.clear();
  retired_framebuffers_.clear();
  // Let the GPU run this frame while the game records the next one: only
  // the previous frame has to be finished (its slot records next).
  frame_ = (frame_ + 1) % kFramesInFlight;
  timings_.worst_texture_frame_ms = std::max(timings_.worst_texture_frame_ms, texture_frame_ms_);
  timings_.worst_buffer_frame_ms = std::max(timings_.worst_buffer_frame_ms, buffer_frame_ms_);
  texture_frame_ms_ = buffer_frame_ms_ = 0.0;
  WaitFrame(frames_[frame_]);
  presented_ = &frames_[frame_];
}

void Renderer::SetGammaRamp(const uint32_t* entries) {
  if (std::memcmp(entries, gamma_ramp_.data(), sizeof(gamma_ramp_))) {
    std::memcpy(gamma_ramp_.data(), entries, sizeof(gamma_ramp_));
    gamma_ramp_dirty_ = true;
  }
}

void Renderer::LoadPipelineCache(const std::string& path) {
  pipeline_cache_path_ = path;
  std::vector<char> data;
  if (FILE* file = std::fopen(path.c_str(), "rb")) {
    std::fseek(file, 0, SEEK_END);
    data.resize(size_t(std::max(std::ftell(file), 0L)));
    std::fseek(file, 0, SEEK_SET);
    data.resize(std::fread(data.data(), 1, data.size(), file));
    std::fclose(file);
  }
  auto* device = static_cast<VulkanDevice*>(device_);
  VkPipelineCacheCreateInfo info{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
  // The driver ignores data from another GPU or version.
  info.initialDataSize = data.size();
  info.pInitialData = data.data();
  if (vkCreatePipelineCache(device->vk, &info, nullptr, &device->pipelineCache) != VK_SUCCESS) {
    info.initialDataSize = 0;
    info.pInitialData = nullptr;
    vkCreatePipelineCache(device->vk, &info, nullptr, &device->pipelineCache);
  }
}

void Renderer::SavePipelineCache() {
  auto* device = static_cast<VulkanDevice*>(device_);
  if (!unsaved_pipelines_ || !device->pipelineCache || pipeline_cache_saving_.exchange(true)) return;
  unsaved_pipelines_ = 0;
  std::thread([this, device] {
    size_t size = 0;
    vkGetPipelineCacheData(device->vk, device->pipelineCache, &size, nullptr);
    std::vector<char> data(size);
    if (vkGetPipelineCacheData(device->vk, device->pipelineCache, &size, data.data()) == VK_SUCCESS) {
      const std::string temporary = pipeline_cache_path_ + ".tmp";
      if (FILE* file = std::fopen(temporary.c_str(), "wb")) {
        const bool written = std::fwrite(data.data(), 1, size, file) == size;
        std::fclose(file);
        if (written) std::rename(temporary.c_str(), pipeline_cache_path_.c_str());
      }
    }
    pipeline_cache_saving_ = false;
  }).detach();
}

Renderer::Submitted Renderer::submitted() const {
  const Frame& frame = frames_[(frame_ + kFramesInFlight - 1) % kFramesInFlight];
  if (!frame.present_ready) return {};
  return {frame.fence.get(), frame.present_pixels, frame.present_width, frame.present_height};
}

void Renderer::WaitSubmitted(const Submitted& submitted) {
  // Not waitForCommandFence, which resets the fence the renderer thread will wait on.
  auto* fence = static_cast<const VulkanCommandFence*>(static_cast<const RenderCommandFence*>(submitted.fence));
  vkWaitForFences(fence->device->vk, 1, &fence->vk, VK_TRUE, UINT64_MAX);
}

Renderer::Shader* Renderer::GetShader(uint64_t key) {
  auto [it, inserted] = shaders_.try_emplace(key);
  Shader& info = it->second;
  if (inserted) {
    // The provider hands over the recompiled shader's SPIR-V (uncompressed).
    std::vector<uint8_t> spirv;
    if (!shader_provider_->Find(key, spirv)) return nullptr;
    info.valid = true;
    info.shader = device_->createShader(spirv.data(), spirv.size(), "main", RenderShaderFormat::SPIRV);
    if (const uint8_t* data = container_ ? container_(key) : nullptr) {
      const uint32_t shader = xenos::LoadBE32(data + 0x18);
      info.microcode_offset = xenos::LoadBE32(data + shader);
      if (xenos::LoadBE32(data) & 1) {
        // VertexShader: field18 at +0x18 and the element count at +0x1C of
        // the Shader struct; elements after field18 dwords from +0x24.
        const uint32_t field18 = xenos::LoadBE32(data + shader + 0x18);
        const uint32_t count = xenos::LoadBE32(data + shader + 0x1C);
        for (uint32_t i = 0; i < count; ++i) {
          const uint32_t value = xenos::LoadBE32(data + shader + 0x24 + 4 * (field18 + i));
          const uint32_t address = value & 0xFFF;
          // The original vfetch's destination register (from the container's
          // microcode): what the recompiled shader reads the element from.
          const uint32_t virtualSize = xenos::LoadBE32(data + 4);
          const uint8_t* instruction = data + virtualSize + info.microcode_offset + address * 12;
          info.elements.push_back({address, (value >> 12) & 0xF, (value >> 16) & 0xF,
                                   (xenos::LoadBE32(instruction) >> 12) & 0x3F,
                                   xenos::LoadBE32(instruction + 4) & 0xFFF});
        }
      }
    }
  }
  return info.valid ? &info : nullptr;
}

uint32_t Renderer::ResolvedIndex(Texture& texture, uint32_t swizzle) {
  if (swizzle == 0x688) return texture.index;  // xyzw (0, 1, 2, 3)
  auto& [view, index] = texture.swizzled_views[swizzle];
  if (!view) {
    RenderTextureViewDesc desc;
    desc.format = texture.format;
    desc.dimension = RenderTextureViewDimension::TEXTURE_2D;
    desc.mipLevels = 1;
    desc.componentMapping = SwizzleMapping(swizzle);
    view = texture.texture->createTextureView(desc);
    if (!index) index = AllocateSlot(1);
    texture_2d_set_->setTexture(index, texture.texture.get(), RenderTextureLayout::SHADER_READ, view.get());
  }
  return index;
}

const RenderBuffer* Renderer::GetGuestBuffer(uint32_t address, uint32_t size, uint64_t layout,
                                             const std::function<void(uint8_t*)>& convert) {
  const uint64_t key = XXH3_64bits_withSeed(&address, 4, layout ^ uint64_t(size) << 32);
  GuestBuffer& cached = guest_buffers_[key];
  if (cached.buffer && !(watcher_ && watcher_->Dirty(address, size, cached.watch_stamp))) {
    static const bool verify = getenv("NATIVE_GPU_VERIFY_TEXTURES") != nullptr;  // debugging
    if (verify) {
      const uint64_t hash = XXH3_64bits(physical_ + address, size);
      if (hash != cached.content_hash) {
        fprintf(stderr, "STALE buffer %08X size %X stamp %u\n", address, size, cached.watch_stamp);
        cached.content_hash = hash;
      }
    }
    return cached.buffer.get();
  }
  struct BufferTimer {
    Renderer* renderer;
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    ~BufferTimer() {
      const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
      renderer->timings_.buffer_ms += ms;
      renderer->buffer_frame_ms_ += ms;
    }
  } buffer_timer{this};
  if (cached.buffer) {
    // A write nearby or of the same data (pages are watched as a whole): keep it.
    const uint64_t hash = XXH3_64bits(physical_ + address, size);
    if (hash == cached.content_hash) {
      cached.watch_stamp = watcher_->Watch(address, size);
      return cached.buffer.get();
    }
    // Written to by the game: the frame may still use the old copy.
    retired_buffers_.push_back(std::move(cached.buffer));
    ++stats_["buffers re-uploaded"];
  } else {
    ++stats_["buffers"];
  }
  if (watcher_) cached.watch_stamp = watcher_->Watch(address, size);
  cached.content_hash = XXH3_64bits(physical_ + address, size);
  cached.buffer = device_->createBuffer(
      RenderBufferDesc::UploadBuffer(std::max(size, 4u), RenderBufferFlag::VERTEX | RenderBufferFlag::INDEX));
  // Converted in cached memory: reading the mapped buffer back is uncached, and
  // that alone took 5 ms a frame on the Deck.
  convert_buffer_.assign(physical_ + address, physical_ + address + size);
  convert(convert_buffer_.data());
  uint8_t* data = static_cast<uint8_t*>(cached.buffer->map());
  std::memcpy(data, convert_buffer_.data(), size);
  cached.buffer->unmap();
  return cached.buffer.get();
}

uint32_t Renderer::GetSampler(const xenos::TextureFetch& t) {
  RenderSamplerDesc desc;
  auto address = [](uint32_t clamp) {
    switch (clamp) {
      case 0: return RenderTextureAddressMode::WRAP;
      case 1: return RenderTextureAddressMode::MIRROR;
      case 3: return RenderTextureAddressMode::MIRROR_ONCE;
      case 6: case 7: return RenderTextureAddressMode::BORDER;
      default: return RenderTextureAddressMode::CLAMP;
    }
  };
  desc.addressU = address(t.clamp[0]);
  desc.addressV = address(t.clamp[1]);
  desc.addressW = address(t.clamp[2]);
  desc.magFilter = t.mag_filter == 1 ? RenderFilter::LINEAR : RenderFilter::NEAREST;
  desc.minFilter = t.min_filter == 1 ? RenderFilter::LINEAR : RenderFilter::NEAREST;
  // Mip filter: 0 point, 1 linear, 2 base map only.
  desc.mipmapMode = t.mip_filter == 1 ? RenderMipmapMode::LINEAR : RenderMipmapMode::NEAREST;
  desc.maxLOD = t.mip_filter == 2 ? 0.0f : 16.0f;
  desc.mipLODBias = t.lod_bias;
  // 0 disabled, 1 to 5: 1:1 to 16:1.
  if (t.aniso_filter >= 2 && t.aniso_filter <= 5) {
    desc.anisotropyEnabled = true;
    desc.maxAnisotropy = 1u << (t.aniso_filter - 1);
  }
  const uint32_t fields[] = {uint32_t(desc.addressU), uint32_t(desc.addressV), uint32_t(desc.addressW),
                             uint32_t(desc.magFilter), uint32_t(desc.minFilter), uint32_t(desc.mipmapMode),
                             uint32_t(desc.maxLOD), uint32_t(t.aniso_filter), uint32_t(int32_t(t.lod_bias * 32.0f))};
  auto [it, inserted] = sampler_indices_.try_emplace(XXH3_64bits(fields, sizeof(fields)), 0);
  if (inserted && samplers_.size() < kSamplerHeapSize) {
    it->second = uint32_t(samplers_.size());
    samplers_.push_back(device_->createSampler(desc));
    sampler_set_->setSampler(it->second, samplers_.back().get());
  }
  return it->second;
}

uint64_t Renderer::TextureContentHash(const xenos::TextureFetch& t, uint32_t byte_size) const {
  uint64_t hash = XXH3_64bits(physical_ + (t.address & 0x1FFFFFFF), byte_size);
  if (t.mip_address) hash ^= XXH3_64bits(physical_ + (t.mip_address & 0x1FFFFFFF), byte_size);
  return hash;
}

Renderer::Texture* Renderer::GetGuestTexture(const xenos::TextureFetch& t) {
  const uint32_t fields[] = {t.format, t.endian, t.tiled, t.pitch, t.address, t.mip_address, t.dimension,
                             t.width, t.height, t.depth, t.swizzle, t.is_signed};
  auto [it, inserted] = guest_textures_.try_emplace(XXH3_64bits(fields, sizeof(fields)));
  Texture& guest = it->second;
  const uint32_t byteSize = xenos::TextureByteSize(t);
  static const bool verifyTextures = getenv("NATIVE_GPU_VERIFY_TEXTURES") != nullptr;  // debugging
  if (!inserted && !verifyTextures && guest.texture &&
      !(watcher_ && (watcher_->Dirty(t.address & 0x1FFFFFFF, byteSize, guest.watch_stamp) ||
                     (t.mip_address && watcher_->Dirty(t.mip_address & 0x1FFFFFFF, byteSize, guest.watch_stamp))))) {
    return &guest;
  }
  struct UploadTimer {
    Renderer* renderer;
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    double elapsed() const {
      return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    }
    ~UploadTimer() {
      const double ms = elapsed();
      renderer->timings_.texture_ms += ms;
      renderer->texture_frame_ms_ += ms;
    }
  } upload_timer{this};
  if (!inserted) {
    if (verifyTextures && guest.texture) {
      const uint64_t hash = TextureContentHash(t, byteSize);
      if (hash != guest.content_hash &&
          !(watcher_ && watcher_->Dirty(t.address & 0x1FFFFFFF, byteSize, guest.watch_stamp))) {
        fprintf(stderr, "STALE %08X %ux%u fmt %u size %X stamp %u\n", t.address, t.width, t.height, t.format,
                byteSize, guest.watch_stamp);
        guest.content_hash = hash;
      }
    }
    if (!guest.texture ||
        !(watcher_ && (watcher_->Dirty(t.address & 0x1FFFFFFF, byteSize, guest.watch_stamp) ||
                       (t.mip_address && watcher_->Dirty(t.mip_address & 0x1FFFFFFF, byteSize, guest.watch_stamp))))) {
      return guest.texture ? &guest : nullptr;
    }
    // Streaming rewrites many textures at once: past the frame's budget, keep
    // the old one for now rather than stall the frame. It stays dirty.
    constexpr double kUpdateBudgetMs = 5.0;
    if (guest.texture && texture_frame_ms_ + upload_timer.elapsed() > kUpdateBudgetMs) {
      ++timings_.textures_deferred;
      return &guest;
    }
    // A write nearby or of the same data (pages are watched as a whole): keep it.
    if (guest.texture) {
      const uint64_t hash = TextureContentHash(t, byteSize);
      if (hash == guest.content_hash) {
        guest.watch_stamp = watcher_->Watch(t.address & 0x1FFFFFFF, byteSize);
        if (t.mip_address) watcher_->Watch(t.mip_address & 0x1FFFFFFF, byteSize);
        ++timings_.textures_unchanged;
        return &guest;
      }
    }
    // Written to by the game: upload again into a new texture, same slot.
    layouts_.erase(guest.texture.get());
    retired_textures_.push_back(std::move(guest.texture));
    retired_views_.push_back(std::move(guest.view));
    if (guest.index) retired_slots_.emplace_back(guest.dimension, guest.index);
    guest.index = 0;
    ++stats_["textures re-uploaded"];
  }
  ++timings_.textures;
  ++texture_generation_;
  // The mip chain lives elsewhere and fits in the size of level 0.
  if (watcher_) {
    const auto watch_start = std::chrono::steady_clock::now();
    guest.watch_stamp = watcher_->Watch(t.address & 0x1FFFFFFF, byteSize);
    if (t.mip_address) watcher_->Watch(t.mip_address & 0x1FFFFFFF, byteSize);
    timings_.texture_watch_ms +=
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - watch_start).count();
  }
  guest.content_hash = TextureContentHash(t, byteSize);
  guest.dimension = t.dimension;
  const RenderFormat format = TextureFormat(t);
  if (t.dimension == 2 && format != RenderFormat::UNKNOWN) {
    // Volume (color grading LUTs).
    const uint64_t size = uint64_t(t.width) * t.height * t.depth * xenos::GetTextureBlock(t.format).bytes;
    const uint64_t at = Allocate(nullptr, size, 512);
    convert_buffer_.resize(size);
    if (!xenos::UntileTexture3D(physical_, t, convert_buffer_.data())) {
      ++stats_["texture skipped: linear or compressed 3D"];
      return nullptr;
    }
    std::memcpy(upload_memory_ + at, convert_buffer_.data(), size);
    guest.format = format;
    guest.width = t.width;
    guest.height = t.height;
    guest.texture = device_->createTexture(RenderTextureDesc::Texture3D(t.width, t.height, t.depth, 1, format));
    Transition(guest.texture.get(), RenderTextureLayout::COPY_DEST);
    commands_->copyTextureRegion(
        RenderTextureCopyLocation::Subresource(guest.texture.get()),
        RenderTextureCopyLocation::PlacedFootprint(upload_, format, t.width, t.height, t.depth, t.width, at));
    Transition(guest.texture.get(), RenderTextureLayout::SHADER_READ);
    RenderTextureViewDesc view;
    view.format = format;
    view.dimension = RenderTextureViewDimension::TEXTURE_3D;
    view.mipLevels = 1;
    view.componentMapping = SwizzleMapping(t.swizzle);
    guest.view = guest.texture->createTextureView(view);
    if (!guest.index) guest.index = AllocateSlot(2);
    texture_3d_set_->setTexture(guest.index, guest.texture.get(), RenderTextureLayout::SHADER_READ, guest.view.get());
    if (inserted) ++stats_["textures (3D)"];
    return &guest;
  }
  if ((t.dimension != 1 && t.dimension != 3) || format == RenderFormat::UNKNOWN) {
    ++stats_["texture skipped: dimension " + std::to_string(t.dimension) + " format " +
             std::to_string(t.format)];
    return nullptr;
  }
  const bool cube = t.dimension == 3;
  const std::vector<xenos::MipLevel> levels = xenos::GetMipLevels(t);
  guest.format = format;
  guest.width = t.width;
  guest.height = t.height;
  const auto create_start = std::chrono::steady_clock::now();
  guest.texture = device_->createTexture(RenderTextureDesc::Texture(
      RenderTextureDimension::TEXTURE_2D, t.width, t.height, 1, uint32_t(levels.size()), cube ? 6 : 1, format,
      cube ? RenderTextureFlag::CUBE : RenderTextureFlag::NONE));
  timings_.texture_create_ms +=
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - create_start).count();
  Transition(guest.texture.get(), RenderTextureLayout::COPY_DEST);
  const xenos::TextureBlock block = xenos::GetTextureBlock(t.format);
  for (uint32_t level = 0; level < levels.size(); ++level) {
    const xenos::MipLevel& m = levels[level];
    const uint32_t blocksX = (m.width + block.size - 1) / block.size;
    const uint32_t blocksY = (m.height + block.size - 1) / block.size;
    for (uint32_t slice = 0; slice < (cube ? 6u : 1u); ++slice) {
      // In cached memory: the upload buffer is slow to write out of order and to
      // read back (the byte swap), which froze the game at each new zone.
      const uint64_t levelSize = uint64_t(blocksX) * blocksY * block.bytes;
      const uint64_t at = Allocate(nullptr, levelSize, 512);
      convert_buffer_.resize(levelSize);
      xenos::UntileLevel(physical_, t, m, slice, convert_buffer_.data());
      std::memcpy(upload_memory_ + at, convert_buffer_.data(), levelSize);
      commands_->copyTextureRegion(
          RenderTextureCopyLocation::Subresource(guest.texture.get(), level, slice),
          RenderTextureCopyLocation::PlacedFootprint(upload_, format, m.width, m.height, 1,
                                                     blocksX * block.size, at));
    }
  }
  Transition(guest.texture.get(), RenderTextureLayout::SHADER_READ);
  if (inserted) ++stats_["texture levels " + std::to_string(levels.size())];
  RenderTextureViewDesc view;
  view.format = format;
  view.dimension = cube ? RenderTextureViewDimension::TEXTURE_CUBE : RenderTextureViewDimension::TEXTURE_2D;
  view.mipLevels = uint32_t(levels.size());
  view.componentMapping = SwizzleMapping(t.swizzle);
  guest.view = guest.texture->createTextureView(view);
  if (!guest.index) guest.index = AllocateSlot(cube ? 3 : 1);
  (cube ? texture_cube_set_ : texture_2d_set_)
      ->setTexture(guest.index, guest.texture.get(), RenderTextureLayout::SHADER_READ, guest.view.get());
  if (inserted) ++stats_[cube ? "textures (cube)" : "textures (2D)"];
  return &guest;
}

Renderer::Texture& Renderer::CreateSurface(uint64_t key, uint32_t width, uint32_t height, RenderFormat format,
                                           bool depth) {
  Texture& surface = surfaces_[key];
  if (surface.texture) return surface;
  surface.format = format;
  surface.width = width;
  surface.height = height;
  surface.texture = device_->createTexture(depth ? RenderTextureDesc::DepthTarget(width, height, format)
                                                 : RenderTextureDesc::ColorTarget(width, height, format));
  RenderTextureViewDesc view;
  view.format = format;
  view.dimension = RenderTextureViewDimension::TEXTURE_2D;
  view.mipLevels = 1;
  surface.view = surface.texture->createTextureView(view);
  surface.index = AllocateSlot(1);
  texture_2d_set_->setTexture(surface.index, surface.texture.get(), RenderTextureLayout::SHADER_READ,
                              surface.view.get());
  if (depth) {
    // plume views a depth target's depth aspect only: swap in a stencil
    // aspect view under a plume view object.
    surface.stencil_view = surface.texture->createTextureView(view);
    auto* vulkanView = static_cast<VulkanTextureView*>(surface.stencil_view.get());
    VkDevice vkDevice = static_cast<VulkanDevice*>(device_)->vk;
    vkDestroyImageView(vkDevice, vulkanView->vk, nullptr);
    VkImageViewCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    info.image = static_cast<VulkanTexture*>(surface.texture.get())->vk;
    info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    info.format = VK_FORMAT_D32_SFLOAT_S8_UINT;
    info.subresourceRange = {VK_IMAGE_ASPECT_STENCIL_BIT, 0, 1, 0, 1};
    vkCreateImageView(vkDevice, &info, nullptr, &vulkanView->vk);
    surface.stencil_index = AllocateSlot(1);
    texture_2d_set_->setTexture(surface.stencil_index, surface.texture.get(), RenderTextureLayout::SHADER_READ,
                                surface.stencil_view.get());
  }
  return surface;
}

// The lowest row the D3D viewport covers.
static uint32_t ViewportBottom(const xenos::DeviceState& state) {
  const float bottom = state.RegF(xenos::reg::PA_CL_VPORT_YOFFSET) +
                       std::fabs(state.RegF(xenos::reg::PA_CL_VPORT_YSCALE));
  return bottom > 0.0f ? uint32_t(std::ceil(bottom)) : 0;
}

// The scene and what works at its size; the small passes stay as they are.
float Renderer::SurfaceScale(uint32_t pitch) const {
  return pitch >= profile_.scene_pitch_min && pitch <= profile_.scene_pitch_max ? render_scale_ : 1.0f;
}
float Renderer::SurfaceWide(uint32_t pitch) const {
  return pitch >= profile_.scene_pitch_min && pitch <= profile_.scene_pitch_max ? render_wide_ : 1.0f;
}

Renderer::Texture& Renderer::GetColorSurface(const xenos::DeviceState& state) {
  const uint32_t surface = state.Reg(xenos::reg::RB_SURFACE_INFO);
  const uint32_t color = state.Reg(xenos::reg::RB_COLOR_INFO);
  const uint32_t pitch = std::max(surface & 0x3FFF, 1u);
  const uint32_t format = (color >> 16) & 0xF;
  const float scale = SurfaceScale(pitch);
  const uint64_t key = uint64_t(color & 0xFFF) | uint64_t(format) << 12 | uint64_t(pitch) << 16 |
                       uint64_t(scale * 4.0f) << 40 | uint64_t(SurfaceWide(pitch) * 16.0f) << 48;
  if (key != last_color_key_ || !last_color_) {
    last_color_ = &CreateSurface(key, uint32_t(pitch * scale * SurfaceWide(pitch)),
                                 uint32_t(kMaxSurfaceHeight * scale), SurfaceFormat(format), false);
    last_color_->scale = scale;
    last_color_->scale_x = scale * SurfaceWide(pitch);
    last_color_key_ = key;
  }
  return *last_color_;
}

Renderer::Texture& Renderer::GetDepthSurface(const xenos::DeviceState& state) {
  const uint32_t surface = state.Reg(xenos::reg::RB_SURFACE_INFO);
  const uint32_t depth = state.Reg(xenos::reg::RB_DEPTH_INFO);
  const uint32_t pitch = std::max(surface & 0x3FFF, 1u);
  const float scale = SurfaceScale(pitch);
  const uint64_t key = uint64_t(depth & 0xFFF) | uint64_t(pitch) << 16 | 1ull << 63 | uint64_t(scale * 4.0f) << 40 |
                       uint64_t(SurfaceWide(pitch) * 16.0f) << 48;
  if (key != last_depth_key_ || !last_depth_) {
    last_depth_ = &CreateSurface(key, uint32_t(pitch * scale * SurfaceWide(pitch)),
                                 uint32_t(kMaxSurfaceHeight * scale), RenderFormat::D32_FLOAT_S8_UINT, true);
    last_depth_->scale = scale;
    last_depth_->scale_x = scale * SurfaceWide(pitch);
    last_depth_key_ = key;
  }
  return *last_depth_;
}

void Renderer::TrackEdram(const xenos::DeviceState& state, Texture& color, Texture& depth, bool colorWritten,
                          bool depthWritten, uint32_t height, bool check) {
  const uint32_t surfaceInfo = state.Reg(xenos::reg::RB_SURFACE_INFO);
  // Draws in a row on the same targets: every tile is already theirs.
  const EdramUse use{&color, &depth, surfaceInfo, state.Reg(xenos::reg::RB_COLOR_INFO),
                     state.Reg(xenos::reg::RB_DEPTH_INFO), height, colorWritten, depthWritten};
  if (check && !std::memcmp(&use, &last_edram_use_, sizeof(use))) return;
  last_edram_use_ = use;
  const uint32_t pitch = std::max(surfaceInfo & 0x3FFF, 1u);
  const uint32_t msaa = (surfaceInfo >> 16) & 3;
  height = std::min(height, kMaxSurfaceHeight);
  // A tile is 80x16 samples of 32 bits (40x16 of 64); 2x MSAA doubles the
  // rows, 4x the columns too.
  const auto tiles = [&](uint32_t info, bool wide, auto&& visit) {
    const uint32_t tileWidth = (wide ? 40 : 80) / (msaa >= 2 ? 2 : 1);
    const uint32_t tileHeight = 16 / (msaa >= 1 ? 2 : 1);
    const uint32_t columns = (pitch + tileWidth - 1) / tileWidth;
    const uint32_t rows = (height + tileHeight - 1) / tileHeight;
    for (uint32_t row = 0; row < rows; ++row) {
      for (uint32_t column = 0; column < columns; ++column) {
        visit((info + row * columns + column) & 2047, column, row, tileWidth, tileHeight);
      }
    }
  };
  if (colorWritten) {
    const uint32_t info = state.Reg(xenos::reg::RB_COLOR_INFO);
    const uint32_t format = (info >> 16) & 0xF;
    const bool wide = format == 5 || format == 7 || format == 15;  // 64 bits per sample
    if (check) {
      // On the GPU those tiles now hold the other surface's data, here this
      // surface's last contents; passes that keep what is in EDRAM (Skate's
      // untiled scene path clears depth only, and foliage writes no alpha)
      // must not get an old frame. Clear them, as the tiled path does.
      std::vector<RenderRect> stale;
      tiles(info & 0xFFF, wide, [&](uint32_t tile, uint32_t column, uint32_t row, uint32_t w, uint32_t h) {
        const Texture* owner = edram_owners_[tile];
        if (!owner || owner == &color || owner == &depth) return;
        const int32_t x = int32_t(column * w * color.scale_x), y = int32_t(row * h * color.scale);
        const int32_t right = int32_t((column + 1) * w * color.scale_x), bottom = int32_t((row + 1) * h * color.scale);
        if (!stale.empty() && stale.back().right == x && stale.back().top == y) {
          stale.back().right = right;
        } else {
          stale.emplace_back(x, y, right, bottom);
        }
      });
      if (!stale.empty()) {
        commands_->clearColor(0, RenderColor(0.0f, 0.0f, 0.0f, 0.0f), stale.data(), uint32_t(stale.size()));
        ++stats_["EDRAM: color target overwritten by another surface"];
      }
    }
    tiles(info & 0xFFF, wide, [&](uint32_t tile, uint32_t, uint32_t, uint32_t, uint32_t) {
      edram_owners_[tile] = &color;
    });
  }
  if (depthWritten) {
    tiles(state.Reg(xenos::reg::RB_DEPTH_INFO) & 0xFFF, false,
          [&](uint32_t tile, uint32_t, uint32_t, uint32_t, uint32_t) { edram_owners_[tile] = &depth; });
  }
}

void Renderer::Clear(const ClearCall& call) {
  Texture& color = GetColorSurface(call.state);
  Texture& depth = GetDepthSurface(call.state);
  SetTargets(color.texture.get(), depth.texture.get());
  RenderRect rect(0, 0, int32_t(color.width), int32_t(color.height));
  if (call.has_rect) {
    rect = RenderRect(int32_t(call.rect[0] * color.scale_x), int32_t(call.rect[1] * color.scale),
                      int32_t(call.rect[2] * color.scale_x), int32_t(call.rect[3] * color.scale));
  }
  TrackEdram(call.state, color, depth, call.flags & 0xF, call.flags & 0x30,
             call.has_rect ? uint32_t(std::max(call.rect[3], 0)) : ViewportBottom(call.state), false);
  if (call.flags & 0xF) {
    commands_->clearColor(0, RenderColor(call.color[0], call.color[1], call.color[2], call.color[3]), &rect, 1);
  }
  if (call.flags & 0x30) {
    // The Z and stencil arguments: RB_DEPTH_CLEAR in the shadow only follows
    // resolve clears.
    commands_->clearDepthStencil(call.flags & 0x10, call.flags & 0x20, call.depth, call.stencil & 0xFF,
                                 &rect, 1);
  }
  ++stats_["clears"];
}

void Renderer::RetireResolved(Texture& dest) {
  // Still referenced by recorded commands: keep it until the frame ends.
  for (auto it = framebuffers_.begin(); it != framebuffers_.end();) {
    if (it->first.first == dest.texture.get()) {
      retired_framebuffers_.push_back(std::move(it->second));
      it = framebuffers_.erase(it);
    } else {
      ++it;
    }
  }
  if (current_targets_.first == dest.texture.get()) current_targets_ = {};
  layouts_.erase(dest.texture.get());
  retired_textures_.push_back(std::move(dest.texture));
  retired_views_.push_back(std::move(dest.view));
  for (auto& [swizzle, entry] : dest.swizzled_views) retired_views_.push_back(std::move(entry.first));
}

void Renderer::Resolve(const ResolveCall& call) {
  const bool depth = call.flags & 4;
  Texture& source = depth ? GetDepthSurface(call.state) : GetColorSurface(call.state);
  const uint32_t address = call.state.Reg(0x2319) & 0xFFFFF000;  // RB_COPY_DEST_BASE
  const uint32_t destPitch = call.state.Reg(0x231A) & 0x3FFF;      // RB_COPY_DEST_PITCH
  const uint32_t destHeight = (call.state.Reg(0x231A) >> 16) & 0x3FFF;
  int32_t rect[4] = {0, 0, int32_t(destPitch), int32_t(destHeight)};
  if (call.has_rect) std::copy(call.rect, call.rect + 4, rect);
  // The source rect is in surface coordinates; the destination point (or
  // the rect's origin) is where it lands. RB_COPY_DEST_PITCH's height counts
  // from that point (predicated tiling resolves the bands one by one).
  const int32_t x = call.has_dest_point ? call.dest_x : rect[0];
  const int32_t y = call.has_dest_point ? call.dest_y : rect[1];
  // Textures are sampled with normalized coordinates, so the host texture has
  // the size the game fetches it at: the destination pitch, unless that is
  // just the width padded to 32 texels.
  const uint32_t rectWidth = uint32_t(rect[2] - rect[0]);
  const uint32_t width = (rect[0] == 0 && destPitch == ((rectWidth + 31) & ~31u)) ? rectWidth : destPitch;
  const uint32_t height = uint32_t(y) + destHeight;
  RenderFormat format = source.format;
  const float scale = source.scale, scaleX = source.scale_x;
  const uint32_t hostWidth = uint32_t(width * scaleX), hostHeight = uint32_t(height * scale);
  ResolvePushConstants push{source.index, 0, 1.0f, int32_t((rect[0] - x) * scaleX), int32_t((rect[1] - y) * scale),
                            source.stencil_index};
  if (depth) {
    format = RenderFormat::R8G8B8A8_UNORM;
    push.mode = 1;
  } else if (format == RenderFormat::R8G8B8A8_UNORM) {
    push.mode = 3;
  } else if (format == RenderFormat::R16G16_FLOAT) {
    // A resolve converts: the EDRAM value (fixed point, -32..32) is written
    // in the destination format, here the unsigned normalized 16_16 the
    // game fetches it as (shadow depth and mask in 0..1).
    format = RenderFormat::R16G16_UNORM;
  }

  Texture& dest = resolved_[address];
  if (!dest.texture || dest.format != format || dest.width != hostWidth || dest.height < hostHeight) {
    ++texture_generation_;
    if (dest.texture) RetireResolved(dest);
    dest.texture = device_->createTexture(RenderTextureDesc::ColorTarget(hostWidth, hostHeight, format));
    dest.format = format;
    dest.width = hostWidth;
    dest.height = hostHeight;
    RenderTextureViewDesc view;
    view.format = format;
    view.dimension = RenderTextureViewDimension::TEXTURE_2D;
    view.mipLevels = 1;
    dest.view = dest.texture->createTextureView(view);
    if (!dest.index) dest.index = AllocateSlot(1);
    texture_2d_set_->setTexture(dest.index, dest.texture.get(), RenderTextureLayout::SHADER_READ,
                                dest.view.get());
  }

  // Draw the source into the destination with the resolve shader.
  Transition(source.texture.get(), RenderTextureLayout::SHADER_READ);
  SetTargets(dest.texture.get(), nullptr);
  const uint64_t key = uint64_t(format) | 1ull << 62;
  auto& pipeline = pipelines_[key];
  if (!pipeline) {
    RenderGraphicsPipelineDesc desc;
    desc.pipelineLayout = resolve_layout_.get();
    desc.vertexShader = resolve_vs_.get();
    desc.pixelShader = resolve_ps_.get();
    desc.renderTargetFormat[0] = format;
    desc.renderTargetBlend[0] = RenderBlendDesc::Copy();
    desc.renderTargetCount = 1;
    pipeline = device_->createGraphicsPipeline(desc);
  }
  commands_->setGraphicsPipelineLayout(resolve_layout_.get());
  commands_->setGraphicsDescriptorSet(texture_2d_set_.get(), 0);
  main_layout_bound_ = false;
  commands_->setPipeline(pipeline.get());
  commands_->setGraphicsPushConstants(0, &push);
  dest.scale = scale;
  dest.scale_x = scaleX;
  RenderViewport viewport(float(x) * scaleX, float(y) * scale, float(rect[2] - rect[0]) * scaleX,
                          float(rect[3] - rect[1]) * scale);
  RenderRect scissor(int32_t(x * scaleX), int32_t(y * scale), int32_t((x + rect[2] - rect[0]) * scaleX),
                     int32_t((y + rect[3] - rect[1]) * scale));
  // The full-screen triangle covers the viewport; the shader reads by pixel.
  viewport = RenderViewport(0.0f, 0.0f, float(dest.width), float(dest.height));
  commands_->setViewports(&viewport, 1);
  commands_->setScissors(&scissor, 1);
  commands_->drawInstanced(3, 1, 0, 0);
  Transition(dest.texture.get(), RenderTextureLayout::SHADER_READ);
  ++stats_[depth ? "resolves (depth)" : "resolves (color)"];

  // Clears that ride on the resolve, limited to the resolved rect (with
  // predicated tiling that is the tile; the other bands are still to resolve).
  if (call.flags & 0x300) {
    ClearCall clear;
    clear.state = call.state;
    clear.has_rect = true;
    std::copy(rect, rect + 4, clear.rect);
    clear.flags = ((call.flags & 0x100) ? 0x1 : 0) | ((call.flags & 0x200) ? 0x10 : 0);
    const uint32_t color = call.state.Reg(0x231E);  // RB_COLOR_CLEAR (8_8_8_8)
    for (int c = 0; c < 4; ++c) clear.color[c] = float((color >> (8 * c)) & 0xFF) / 255.0f;
    clear.depth = call.clear_depth;
    clear.flags |= (call.flags & 0x200) ? 0x20 : 0;
    Clear(clear);
  }
}

void Renderer::Draw(const DrawCall& call) {
  const xenos::DeviceState& state = call.state;
  RenderPrimitiveTopology topology;
  bool quads = false;
  switch (call.primitive) {
    case 1: topology = RenderPrimitiveTopology::POINT_LIST; break;
    case 2: topology = RenderPrimitiveTopology::LINE_LIST; break;
    case 3: topology = RenderPrimitiveTopology::LINE_STRIP; break;
    case 4: topology = RenderPrimitiveTopology::TRIANGLE_LIST; break;
    case 6: topology = RenderPrimitiveTopology::TRIANGLE_STRIP; break;
    case 13:  // quad list: expanded to triangles below
      topology = RenderPrimitiveTopology::TRIANGLE_LIST;
      quads = true;
      break;
    default:
      ++stats_["draw skipped: primitive " + std::to_string(call.primitive)];
      return;
  }
  if (quads && call.indices_size) {
    ++stats_["draw skipped: indexed quad list"];
    return;
  }
  Shader* vs = GetShader(call.vs_key);
  Shader* ps = GetShader(call.ps_key);
  if (!vs || !ps) {
    ++stats_["draw skipped: shader not in cache"];
    return;
  }

  // Vertex input from the patched vfetch instructions, decoded once for each
  // shader and set of fetches.
  // 16-bit elements per slot (byte offset, dwords): Xenos keeps the first
  // component in the high half of each dword, so after the 32-bit swap their
  // halves are put back in order.
  // (VertexLayout::half_swaps; narrowed holds the 10_11_11 elements fed to float
  // inputs per slot, byte offset and signed, packed the 8_8_8_8 normals.)
  uint32_t lastStride = 0, lastSlot = 0;
  // What the GPU loads is the object's physical part as D3D patched it.
  static const bool traceSkips = getenv("NATIVE_GPU_TRACE_SKIPS") != nullptr;  // debugging
  const auto traceSkip = [&](const char* why) {
    if (traceSkips) {
      fprintf(stderr, "SKIP %s vs %016llX ps %016llX prim %u count %u\n", why, (unsigned long long)call.vs_key,
              (unsigned long long)call.ps_key, call.primitive, call.count);
    }
  };
  if (!call.vs_physical) {
    ++stats_["draw skipped: no vertex microcode"];
    return;
  }
  const uint8_t* microcode = call.vs_physical + vs->microcode_offset;
  const size_t microcodeSize = call.vs_physical_size - std::min<size_t>(call.vs_physical_size, vs->microcode_offset);
  size_t fetchesEnd = 0;
  for (const Shader::Element& element : vs->elements) fetchesEnd = std::max<size_t>(fetchesEnd, (element.address + 1) * 12);
  const uint64_t layoutKey = XXH3_64bits_withSeed(microcode, std::min(fetchesEnd, microcodeSize), call.vs_key);
  VertexLayout& vertexLayout = vertex_layouts_[layoutKey];
  const bool freshLayout = !vertexLayout.valid;
  if (freshLayout) vertexLayout = VertexLayout();
  std::vector<RenderInputElement>& inputElements = vertexLayout.elements;
  std::vector<RenderInputSlot>& inputSlots = vertexLayout.slots;
  std::vector<uint32_t>& slotFetch = vertexLayout.slot_fetch;
  auto& halfSwaps = vertexLayout.half_swaps;
  auto& narrowed = vertexLayout.narrowed;
  auto& packed = vertexLayout.packed;
  bool& packedNormals = vertexLayout.packed_normals;
  bool& halfNormals = vertexLayout.half_normals;
  if (freshLayout) for (const Shader::Element& element : vs->elements) {
    // D3D relinks the vertex fetches it loads: registers may move between
    // fetch instructions, and swizzles change (x and y swapped for its 16-bit
    // pairs). Use the fetch writing the element's original destination register
    // and components, among the elements' fetch slots.
    const auto written = [](uint32_t swizzle) {
      uint32_t mask = 0;
      for (uint32_t c = 0; c < 4; ++c) mask |= uint32_t(((swizzle >> (3 * c)) & 7) != 7) << c;
      return mask;
    };
    size_t at = element.address * 12;
    for (const Shader::Element& other : vs->elements) {
      const size_t candidate = other.address * 12;
      if (candidate + 12 <= microcodeSize &&
          ((xenos::LoadBE32(microcode + candidate) >> 12) & 0x3F) == element.dst_register &&
          written(xenos::LoadBE32(microcode + candidate + 4) & 0xFFF) == written(element.dst_swizzle) &&
          (xenos::LoadBE32(microcode + candidate) & 0x1F) == 0) {
        at = candidate;
        break;
      }
    }
    if (at + 12 > microcodeSize) {
      ++stats_["draw skipped: vertex fetch outside microcode"];
      return;
    }
    xenos::VertexFetchInstruction fetch = xenos::DecodeVertexFetchInstruction(microcode + at);
    if (!fetch.is_vfetch) {
      ++stats_["draw skipped: vertex element is not a vfetch"];
      traceSkip("notvfetch");
      return;
    }
    if (fetch.is_mini) {
      // A mini fetch takes the stride and slot of the full fetch before it
      // in the microcode.
      fetch.stride = lastStride;
      fetch.fetch_slot = lastSlot;
      for (size_t back = at; back >= 12;) {
        back -= 12;
        const xenos::VertexFetchInstruction full = xenos::DecodeVertexFetchInstruction(microcode + back);
        if (full.is_vfetch && !full.is_mini) {
          fetch.stride = full.stride;
          fetch.fetch_slot = full.fetch_slot;
          break;
        }
      }
    } else {
      lastStride = fetch.stride;
      lastSlot = fetch.fetch_slot;
    }
    if (fetch.exp_adjust) {
      ++stats_["vertex exp_adjust " + std::to_string(fetch.exp_adjust) + ": usage " +
               std::to_string(element.usage) + "." + std::to_string(element.usage_index) + " format " +
               std::to_string(fetch.format)];
    }
    // A fetch of constants only (0 and 1 in its swizzle, no vertex data): vertices
    // without this attribute, such as the wires' normals. Left unbound, it reads 0.
    bool fetchesData = false;
    for (uint32_t c = 0; c < 4; ++c) {
      fetchesData |= ((xenos::LoadBE32(microcode + at + 4) >> (3 * c)) & 7) < 4;
    }
    if (!fetchesData) continue;
    const bool raw = IsRawUsage(element.usage);
    const RenderFormat format = raw ? RawVertexFormat(fetch.format) : VertexFormat(fetch, element.usage);
    if (raw && (fetch.format == xenos::k_10_11_11 || fetch.format == xenos::k_8_8_8_8)) packedNormals = true;
    if (raw && (fetch.format == xenos::k_16_16_FLOAT || fetch.format == xenos::k_16_16_16_16_FLOAT)) {
      halfNormals = true;
    }
    const int32_t location = UsageLocation(element.usage, element.usage_index);
    if (format == RenderFormat::UNKNOWN || location < 0) {
      ++stats_[(location < 0 ? "vertex element without a location: usage " : "vertex element format: usage ") +
               std::to_string(element.usage) + "." + std::to_string(element.usage_index) + " format " +
               std::to_string(fetch.format) + (fetch.is_signed ? " signed" : "") +
               (fetch.is_integer ? " integer" : "")];
      continue;
    }
    uint32_t slot = 0;
    while (slot < slotFetch.size() && slotFetch[slot] != fetch.fetch_slot) ++slot;
    if (slot == slotFetch.size()) {
      slotFetch.push_back(fetch.fetch_slot);
      inputSlots.emplace_back(slot, fetch.stride * 4);
      halfSwaps.emplace_back();
      narrowed.emplace_back();
      packed.emplace_back();
    }
    if (raw && fetch.format == xenos::k_8_8_8_8) {
      packed[slot].emplace_back(uint32_t(fetch.offset * 4), uint32_t(fetch.is_signed));
    }
    if (fetch.format == xenos::k_10_11_11 && !raw) {
      narrowed[slot].emplace_back(uint32_t(fetch.offset * 4), uint32_t(fetch.is_signed));
    }
    if (fetch.format == xenos::k_16_16 || fetch.format == xenos::k_16_16_FLOAT) {
      halfSwaps[slot].emplace_back(uint32_t(fetch.offset * 4), 1);
    } else if (fetch.format == xenos::k_16_16_16_16 || fetch.format == xenos::k_16_16_16_16_FLOAT) {
      halfSwaps[slot].emplace_back(uint32_t(fetch.offset * 4), 2);
    }
    inputElements.emplace_back("", 0, uint32_t(location), format, slot, uint32_t(fetch.offset * 4));
  }
  vertexLayout.valid = true;
  vertexLayout.slot_layouts.resize(slotFetch.size());
  std::vector<RenderVertexBufferView>& vertexViews = vertex_views_;
  vertexViews.clear();
  for (uint32_t slot = 0; slot < slotFetch.size(); ++slot) {
    const xenos::VertexFetch fetch = xenos::DecodeVertexFetch(state, slotFetch[slot]);
    const uint32_t stride = inputSlots[slot].stride;
    auto convert = [&](uint8_t* data, uint32_t size, uint32_t endian) {
      xenos::SwapEndian(data, size, endian);
      for (const auto& [offset, is_signed] : narrowed[slot]) {
        for (uint32_t vertex = 0; stride && (vertex + 1) * stride <= size; ++vertex) {
          uint8_t* p = data + vertex * stride + offset;
          uint32_t v;
          std::memcpy(&v, p, 4);
          // x and y are 11 bits, z 10, in that order from the low bit.
          p[0] = uint8_t((v >> 3) & 0xFF);
          p[1] = uint8_t((v >> 14) & 0xFF);
          p[2] = uint8_t(v >> 24);
          p[3] = is_signed ? 127 : 255;
        }
      }
      // 8_8_8_8 normals, in the 10_11_11 layout the shaders' normal fetch decodes.
      for (const auto& [offset, is_signed] : packed[slot]) {
        const auto scaled = [is_signed = is_signed](uint32_t byte, int32_t one, uint32_t mask) {
          const float value = is_signed ? std::max(float(int8_t(byte)) / 127.0f, -1.0f) : float(byte) / 255.0f;
          return uint32_t(std::clamp(int32_t(std::lround(value * float(one))), -one, one - 1)) & mask;
        };
        for (uint32_t vertex = 0; stride && (vertex + 1) * stride <= size; ++vertex) {
          uint8_t* p = data + vertex * stride + offset;
          const uint32_t v = scaled(p[0], 1024, 0x7FF) | scaled(p[1], 1024, 0x7FF) << 11 | scaled(p[2], 512, 0x3FF) << 22;
          std::memcpy(p, &v, 4);
        }
      }
      for (const auto& [offset, dwords] : halfSwaps[slot]) {
        for (uint32_t vertex = 0; stride && (vertex + 1) * stride <= size; ++vertex) {
          uint8_t* p = data + vertex * stride + offset;
          for (uint32_t d = 0; d < dwords; ++d, p += 4) {
            std::swap(p[0], p[2]);
            std::swap(p[1], p[3]);
          }
        }
      }
    };
    if (call.inline_vertices) {
      // Inline vertices live in the command buffer: converted every time.
      convert_buffer_.assign(call.inline_vertices, call.inline_vertices + call.inline_size);
      convert(convert_buffer_.data(), call.inline_size, 2);
      const uint64_t at = Allocate(convert_buffer_.data(), call.inline_size, 16);
      vertexViews.push_back({upload_->at(at), call.inline_size});
      continue;
    }
    // Guest vertex buffers are converted once and kept until written to.
    if (freshLayout) {
      uint64_t layout = XXH3_64bits_withSeed(halfSwaps[slot].data(),
                                             halfSwaps[slot].size() * sizeof(halfSwaps[slot][0]), stride);
      layout ^= XXH3_64bits(narrowed[slot].data(), narrowed[slot].size() * sizeof(narrowed[slot][0]));
      layout ^= XXH3_64bits_withSeed(packed[slot].data(), packed[slot].size() * sizeof(packed[slot][0]), 1);
      vertexLayout.slot_layouts[slot] = layout;
    }
    const uint32_t address = fetch.address & 0x1FFFFFFF;
    const auto fill = [&](uint8_t* data) { convert(data, fetch.size, fetch.endian); };
    const RenderBuffer* buffer =
        GetGuestBuffer(address, fetch.size, vertexLayout.slot_layouts[slot] ^ fetch.endian, std::ref(fill));
    vertexViews.push_back({buffer, fetch.size});
  }

  // Textures (resolved ones by address first) and samplers.
  SharedConstants shared;
  for (uint32_t slot = 0; slot < 16; ++slot) {
    // A fetch constant already seen, with no texture changed since.
    const uint8_t* fetchConstant = state.data + xenos::device::kFetchConstants + 24 * slot;
    uint32_t words[3];
    std::memcpy(words, fetchConstant, 12);
    SlotBinding& binding = bindings_[((words[0] * 2654435761u) ^ (words[1] * 40503u) ^ words[2]) >> 22];
    if (binding.generation == texture_generation_ && !std::memcmp(binding.fetch, fetchConstant, 24)) {
      shared.texture2DIndices[slot] = binding.texture_2d;
      shared.texture3DIndices[slot] = binding.texture_3d;
      shared.textureCubeIndices[slot] = binding.texture_cube;
      shared.samplerIndices[slot] = binding.sampler;
      continue;
    }
    const xenos::TextureFetch t = xenos::DecodeTextureFetch(state, slot);
    if (!t.valid) continue;
    auto resolved = resolved_.find(t.address);
    if (resolved != resolved_.end() && watcher_ &&
        watcher_->Dirty(t.address & 0x1FFFFFFF, resolved->second.resolved_size,
                        resolved->second.watch_stamp)) {
      // Written by the game since it was resolved: the guest texture now.
      RetireResolved(resolved->second);
      if (resolved->second.index) retired_slots_.emplace_back(1, resolved->second.index);
      for (const auto& [swizzle, entry] : resolved->second.swizzled_views) {
        if (entry.second) retired_slots_.emplace_back(1, entry.second);
      }
      resolved_.erase(resolved);
      resolved = resolved_.end();
      ++texture_generation_;
      ++stats_["resolved textures overwritten by the game"];
    }
    if (resolved != resolved_.end()) {
      shared.texture2DIndices[slot] = ResolvedIndex(resolved->second, t.swizzle);
    } else if (Texture* texture = GetGuestTexture(t)) {
      (texture->dimension == 3   ? shared.textureCubeIndices
       : texture->dimension == 2 ? shared.texture3DIndices
                                 : shared.texture2DIndices)[slot] = texture->index;
    }
    shared.samplerIndices[slot] = GetSampler(t);
    std::memcpy(binding.fetch, fetchConstant, 24);
    binding.generation = texture_generation_;
    binding.texture_2d = shared.texture2DIndices[slot];
    binding.texture_3d = shared.texture3DIndices[slot];
    binding.texture_cube = shared.textureCubeIndices[slot];
    binding.sampler = shared.samplerIndices[slot];
  }

  // Targets.
  Texture& color = GetColorSurface(state);
  Texture& depth = GetDepthSurface(state);
  SetTargets(color.texture.get(), depth.texture.get());
  SetMainLayout();

  // Pipeline.
  const uint32_t depthControl = state.Reg(xenos::reg::RB_DEPTHCONTROL);
  const uint32_t blendControl = state.Reg(xenos::reg::RB_BLENDCONTROL0);
  const uint32_t colorControl = state.Reg(xenos::reg::RB_COLORCONTROL);
  RenderGraphicsPipelineDesc desc;
  desc.pipelineLayout = layout_.get();
  desc.vertexShader = vs->shader.get();
  desc.pixelShader = ps->shader.get();
  desc.depthEnabled = (depthControl >> 1) & 1;
  desc.depthWriteEnabled = (depthControl >> 2) & 1;
  desc.depthFunction = RenderComparisonFunction(((depthControl >> 4) & 7) + 1);
  // Stencil (RB_DEPTHCONTROL bit 0; front func/fail/zpass/zfail at 8/11/14/17,
  // back at 20/23/26/29 when bit 7; RB_STENCILREFMASK ref/mask/writemask).
  desc.stencilEnabled = depthControl & 1;
  if (desc.stencilEnabled) {
    const uint32_t refMask = state.Reg(0x210D);
    desc.stencilReference = refMask & 0xFF;
    desc.stencilReadMask = (refMask >> 8) & 0xFF;
    desc.stencilWriteMask = (refMask >> 16) & 0xFF;
    desc.stencilFrontFace = StencilFace((depthControl >> 8) & 7, (depthControl >> 11) & 7,
                                        (depthControl >> 14) & 7, (depthControl >> 17) & 7);
    desc.stencilBackFace = (depthControl >> 7) & 1
                               ? StencilFace((depthControl >> 20) & 7, (depthControl >> 23) & 7,
                                             (depthControl >> 26) & 7, (depthControl >> 29) & 7)
                               : desc.stencilFrontFace;
  }
  desc.depthClipEnabled = true;
  desc.dynamicDepthBiasEnabled = true;
  desc.primitiveTopology = topology;
  // PA_SU_SC_MODE_CNTL: cull front, cull back, and the front face (clockwise when
  // set), all seen through the flipped viewport plume is always clockwise for.
  const uint32_t cullBits = state.Reg(xenos::reg::PA_SU_SC_MODE_CNTL) & 3;
  const bool frontClockwise = state.Reg(xenos::reg::PA_SU_SC_MODE_CNTL) & 4;
  if (cullBits == 0 || cullBits == 3) {
    desc.cullMode = RenderCullMode::NONE;
  } else {
    desc.cullMode = (cullBits == 1) == frontClockwise ? RenderCullMode::FRONT : RenderCullMode::BACK;
  }
  desc.renderTargetFormat[0] = color.format;
  desc.renderTargetCount = 1;
  desc.depthTargetFormat = depth.format;
  RenderBlendDesc& blend = desc.renderTargetBlend[0];
  blend.srcBlend = BlendFactor(blendControl & 0x1F);
  blend.blendOp = BlendOp((blendControl >> 5) & 7);
  blend.dstBlend = BlendFactor((blendControl >> 8) & 0x1F);
  blend.srcBlendAlpha = BlendFactor((blendControl >> 16) & 0x1F);
  blend.blendOpAlpha = BlendOp((blendControl >> 21) & 7);
  blend.dstBlendAlpha = BlendFactor((blendControl >> 24) & 0x1F);
  blend.blendEnabled = !(blend.srcBlend == RenderBlend::ONE && blend.dstBlend == RenderBlend::ZERO &&
                         blend.blendOp == RenderBlendOperation::ADD &&
                         blend.srcBlendAlpha == RenderBlend::ONE && blend.dstBlendAlpha == RenderBlend::ZERO &&
                         blend.blendOpAlpha == RenderBlendOperation::ADD);
  blend.renderTargetWriteMask = uint8_t(state.Reg(xenos::reg::RB_COLOR_MASK) & 0xF);
  uint32_t specConstants = packedNormals ? 1 : 0;  // SPEC_CONSTANT_R11G11B10_NORMAL
  if (halfNormals) specConstants |= 1 << 5;          // SPEC_CONSTANT_HALF_NORMAL
  if ((colorControl >> 3) & 1) specConstants |= 2;  // SPEC_CONSTANT_ALPHA_TEST
  RenderSpecConstant spec(0, specConstants);
  desc.specConstants = &spec;
  desc.specConstantsCount = 1;
  desc.inputSlots = inputSlots.data();
  desc.inputSlotsCount = uint32_t(inputSlots.size());
  desc.inputElements = inputElements.data();
  desc.inputElementsCount = uint32_t(inputElements.size());
  uint64_t key = XXH3_64bits(&desc, offsetof(RenderGraphicsPipelineDesc, inputSlots));
  key = XXH3_64bits_withSeed(inputSlots.data(), inputSlots.size() * sizeof(RenderInputSlot), key);
  for (const RenderInputElement& element : inputElements) {
    const uint32_t fields[] = {element.location, uint32_t(element.format), element.slotIndex,
                               element.alignedByteOffset};
    key = XXH3_64bits_withSeed(fields, sizeof(fields), key);
  }
  key = XXH3_64bits_withSeed(&specConstants, 4, key);
  auto& pipeline = pipelines_[key];
  if (!pipeline) {
    const auto start = std::chrono::steady_clock::now();
    pipeline = device_->createGraphicsPipeline(desc);
    ++pipelines_created_;
    ++unsaved_pipelines_;
    pipeline_ms_ += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
  }
  if (!pipeline) {
    ++stats_["draw skipped: pipeline creation"];
    return;
  }

  // Constants.
  // Draws in a row often share them: keep the upload.
  const auto uploadConstants = [&](ConstantUpload& last, const uint8_t* source) {
    if (last.upload != upload_ || std::memcmp(last.source.data(), source, 4096)) {
      last.offset = Allocate(nullptr, 4096);
      last.upload = upload_;
      std::memcpy(last.source.data(), source, 4096);
      CopySwapped32(upload_memory_ + last.offset, source, 4096);
    }
    return last.offset;
  };
  const bool worldDraw = color.scale_x > color.scale && desc.depthEnabled &&
                         !profile_.IsInterface(call.vs_key);
  const uint64_t vsConstants = uploadConstants(vs_constants_, state.data + xenos::device::kVertexConstants);
  const uint64_t psConstants = uploadConstants(ps_constants_, state.data + xenos::device::kPixelConstants);
  shared.booleans = (state.U32(xenos::device::kVertexBoolConstants) & 0xFF) |
                    ((state.U32(xenos::device::kPixelBoolConstants) & 0xFF) << 16);
  shared.alphaThreshold = state.RegF(xenos::reg::RB_ALPHA_REF);
  const uint64_t sharedConstants = Allocate(&shared, sizeof(shared));
  const uint64_t pushConstants[] = {upload_address_ + vsConstants, upload_address_ + psConstants,
                                    upload_address_ + sharedConstants};

  TrackEdram(state, color, depth, blend.renderTargetWriteMask != 0,
             desc.depthWriteEnabled || (desc.stencilEnabled && desc.stencilWriteMask), ViewportBottom(state),
             true);

  // Viewport from the D3D viewport transform (the shaders invert Y).
  // The interface keeps its 16:9 shape, in the middle of a wider picture.
  bool interfaceDraw = profile_.IsInterface(call.vs_key);
  // Except what covers the whole screen, like the dimming behind the pause menu.
  if (interfaceDraw && call.inline_vertices && !inputSlots.empty() && call.count <= 6) {
    const uint32_t stride = inputSlots[0].stride;
    float left = FLT_MAX, right = -FLT_MAX;
    for (uint32_t vertex = 0; stride && vertex < call.count && (vertex + 1) * stride <= call.inline_size; ++vertex) {
      const float x = xenos::LoadBEFloat(call.inline_vertices + vertex * stride);
      left = std::min(left, x);
      right = std::max(right, x);
    }
    if (right - left >= 1200.0f) {
      interfaceDraw = false;
      interface_cover_ = true;
    }
  }
  const float xScale = interfaceDraw ? color.scale : color.scale_x;
  // The spare width beside the 16:9 area. Over the 3D world, the interface's
  // sides follow the screen's sides: the transform in c4 gives the position on a 1280 wide screen.
  // (Menus stay together in the middle: without the world, or after a cover over it.)
  float xOffset = 0.0f;
  if (interfaceDraw && color.scale_x > color.scale) {
    const float spare = (color.scale_x - color.scale) * float(color.width / color.scale_x);
    const uint8_t* transform = state.data + xenos::device::kVertexConstants + 4 * 16;
    float screenX = xenos::LoadBEFloat(transform + 12);
    if (call.inline_vertices && call.inline_size >= 8) {
      screenX += xenos::LoadBEFloat(transform) * xenos::LoadBEFloat(call.inline_vertices) +
                 xenos::LoadBEFloat(transform + 4) * xenos::LoadBEFloat(call.inline_vertices + 4);
    }
    constexpr uint32_t kWorldFrame = 50;
    const bool world = world_draws_last_frame_ >= kWorldFrame && !interface_cover_;
    // The minimap's shader has no such position: it lives in the top right corner.
    const bool minimap = profile_.IsCornerWidget(call.vs_key);
    xOffset = !world ? spare * 0.5f : minimap || screenX > 880.0f ? spare : screenX < 400.0f ? 0.0f : spare * 0.5f;
  }
  world_draws_ += worldDraw;
  const float sx = state.RegF(xenos::reg::PA_CL_VPORT_XSCALE) * xScale;
  // Direct3D 9 pixel centers (pix_center 0): half a pixel, or every full-screen
  // pass samples between texels and blurs the frame.
  const float half = (state.Reg(xenos::reg::PA_SU_VTX_CNTL) & 1) ? 0.0f : 0.5f;
  const float scaleXy = color.scale;
  const float ox = state.RegF(xenos::reg::PA_CL_VPORT_XOFFSET) * xScale + xOffset + half;
  const float sy = state.RegF(xenos::reg::PA_CL_VPORT_YSCALE) * scaleXy;
  const float oy = state.RegF(xenos::reg::PA_CL_VPORT_YOFFSET) * scaleXy + half;
  const float sz = state.RegF(xenos::reg::PA_CL_VPORT_ZSCALE);
  const float oz = state.RegF(xenos::reg::PA_CL_VPORT_ZOFFSET);
  RenderViewport viewport(ox - sx, oy + sy, 2.0f * sx, -2.0f * sy, oz, oz + sz);
  const float boundViewport[] = {ox, oy, sx, sy, oz, sz, float(color.width), float(color.height)};
  if (!has_bound_viewport_ || std::memcmp(bound_viewport_, boundViewport, sizeof(boundViewport))) {
    commands_->setViewports(&viewport, 1);
    RenderRect scissor(0, 0, int32_t(color.width), int32_t(color.height));
    commands_->setScissors(&scissor, 1);
    std::memcpy(bound_viewport_, boundViewport, sizeof(boundViewport));
    has_bound_viewport_ = true;
  }

  if (bound_pipeline_ != pipeline.get()) {
    commands_->setPipeline(pipeline.get());
    bound_pipeline_ = pipeline.get();
  }
  // Polygon offset, or coplanar layers (decals, ground) fight in the distance.
  // Enabled per face; the game sets both alike. The offset is in depth units,
  // and a D32F constant unit is 2^-24 at the far depths where this matters.
  const uint32_t mode = state.Reg(xenos::reg::PA_SU_SC_MODE_CNTL);
  const bool front_offset = mode & (1u << 11), back_offset = mode & (1u << 12);
  if (front_offset || back_offset) {
    const uint32_t base = front_offset ? xenos::reg::PA_SU_POLY_OFFSET_FRONT_SCALE
                                       : xenos::reg::PA_SU_POLY_OFFSET_BACK_SCALE;
    const float bias[2] = {state.RegF(base + 1) * 16777216.0f, state.RegF(base) / 16.0f};
    if (!has_bound_bias_ || std::memcmp(bound_bias_, bias, sizeof(bias))) {
      commands_->setDepthBias(bias[0], 0.0f, bias[1]);
      std::memcpy(bound_bias_, bias, sizeof(bias));
    }
    has_bound_bias_ = true;
    ++stats_["draws with polygon offset"];
  } else if (!has_bound_bias_ || bound_bias_[0] != 0.0f || bound_bias_[1] != 0.0f) {
    commands_->setDepthBias(0.0f, 0.0f, 0.0f);
    bound_bias_[0] = bound_bias_[1] = 0.0f;
    has_bound_bias_ = true;
  }
  commands_->setGraphicsPushConstants(0, pushConstants);
  commands_->setVertexBuffers(0, vertexViews.data(), uint32_t(vertexViews.size()), inputSlots.data());
  if (call.indices_size) {
    const auto fill = [&](uint8_t* data) { xenos::SwapEndian(data, call.indices_size, call.indices_32bit ? 2 : 1); };
    const RenderFormat indexFormat = call.indices_32bit ? RenderFormat::R32_UINT : RenderFormat::R16_UINT;
    RenderIndexBufferView view;
    if (call.inline_indices) {
      // User memory indices: converted at every draw into the frame's upload buffer.
      convert_buffer_.assign(call.inline_indices, call.inline_indices + call.indices_size);
      fill(convert_buffer_.data());
      const uint64_t at = Allocate(convert_buffer_.data(), call.indices_size, 16);
      view = RenderIndexBufferView{upload_->at(at), call.indices_size, indexFormat};
    } else {
      const RenderBuffer* buffer =
          GetGuestBuffer(call.indices_address, call.indices_size, call.indices_32bit ? 2 : 1, std::ref(fill));
      view = RenderIndexBufferView{buffer, call.indices_size, indexFormat};
    }
    commands_->setIndexBuffer(&view);
    commands_->drawIndexedInstanced(call.count, 1, call.start, int32_t(call.base_vertex), 0);
  } else if (quads) {
    const uint32_t quadCount = call.count / 4;
    std::vector<uint16_t> indices;
    for (uint32_t q = 0; q < quadCount; ++q) {
      const uint16_t v = uint16_t(call.start + 4 * q);
      for (uint16_t i : {0, 1, 2, 0, 2, 3}) indices.push_back(uint16_t(v + i));
    }
    const uint64_t at = Allocate(indices.data(), indices.size() * 2, 16);
    RenderIndexBufferView view{upload_->at(at), uint32_t(indices.size() * 2), RenderFormat::R16_UINT};
    commands_->setIndexBuffer(&view);
    commands_->drawIndexedInstanced(uint32_t(indices.size()), 1, 0, 0, 0);
  } else {
    commands_->drawInstanced(call.count, 1, call.start, 0);
  }
  ++stats_["draws"];
}

bool Renderer::ReadResolved(uint32_t address, std::vector<uint8_t>& rgba, uint32_t& width, uint32_t& height) {
  auto it = resolved_.find(address & 0xFFFFF000);
  if (it == resolved_.end() || (it->second.format != RenderFormat::R8G8B8A8_UNORM &&
                                it->second.format != RenderFormat::R16G16_UNORM)) {
    return false;
  }
  Texture& texture = it->second;
  width = texture.width;
  height = texture.height;
  auto readback = device_->createBuffer(RenderBufferDesc::ReadbackBuffer(uint64_t(width) * height * 4));
  WaitIdle();
  Frame& frame = frames_[frame_];
  commands_ = frame.commands.get();
  commands_->begin();
  layouts_.erase(texture.texture.get());
  layouts_[texture.texture.get()] = RenderTextureLayout::SHADER_READ;
  Transition(texture.texture.get(), RenderTextureLayout::COPY_SOURCE);
  // plume has no texture-to-buffer copy.
  VkBufferImageCopy region{};
  region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  region.imageSubresource.layerCount = 1;
  region.imageExtent = {width, height, 1};
  vkCmdCopyImageToBuffer(static_cast<VulkanCommandList*>(commands_)->vk,
                         static_cast<VulkanTexture*>(texture.texture.get())->vk,
                         VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, static_cast<VulkanBuffer*>(readback.get())->vk,
                         1, &region);
  Transition(texture.texture.get(), RenderTextureLayout::SHADER_READ);
  commands_->end();
  const RenderCommandList* lists[] = {commands_};
  queue_->executeCommandLists(lists, 1, nullptr, 0, nullptr, 0, frame.fence.get());
  queue_->waitForCommandFence(frame.fence.get());
  const uint8_t* data = static_cast<const uint8_t*>(readback->map());
  rgba.assign(data, data + size_t(width) * height * 4);
  readback->unmap();
  return true;
}

}  // namespace native_gpu

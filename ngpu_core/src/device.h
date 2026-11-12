// SPDX-License-Identifier: GPL-3.0-or-later
// ngpu_core - creation du peripherique Vulkan (plume) et assemblage noyau = peripherique + moteur + source de shaders.
// Piece nouvelle de XenonDash ; la creation du peripherique reprend native_gpu.cpp de Skate1Native (commit 81805f51).

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include <plume_render_interface.h>

#include "profile.h"
#include "renderer.h"
#include "shader_provider.h"

namespace native_gpu {

// Un noyau de rendu pret a l'emploi : un peripherique Vulkan, un moteur et une source de shaders.
class Core {
 public:
  Core();
  ~Core();
  // `physical` : memoire physique de l'invite (la base de 512 Mo) ; `container` : retrouve, par cle de microcode, le
  // conteneur de shader (grand-boutiste, partie virtuelle puis physique) copie a la creation de l'objet.
  bool Initialize(const uint8_t* physical, std::function<const uint8_t*(uint64_t key)> container,
                  WriteWatcher* watcher, const Profile& profile, std::string* error = nullptr);
  Renderer& renderer() { return renderer_; }
  MapShaderProvider& shaders() { return shaders_; }
  const std::string& device_name() const { return device_name_; }

 private:
  std::unique_ptr<plume::RenderInterface> interface_;
  std::unique_ptr<plume::RenderDevice> device_;
  MapShaderProvider shaders_;
  Renderer renderer_;
  std::string device_name_;
};

}  // namespace native_gpu

// SPDX-License-Identifier: GPL-3.0-or-later
// ngpu_core - voir device.h.

#include "device.h"

namespace plume {
std::unique_ptr<RenderInterface> CreateVulkanInterface();
}

namespace native_gpu {

Core::Core() = default;
Core::~Core() {
  // Le moteur doit etre arrete avant le peripherique.
  renderer_.WaitIdle();
}

bool Core::Initialize(const uint8_t* physical, std::function<const uint8_t*(uint64_t key)> container,
                      WriteWatcher* watcher, const Profile& profile, std::string* error) {
  interface_ = plume::CreateVulkanInterface();
  if (!interface_) {
    if (error) *error = "pas d'interface Vulkan";
    return false;
  }
  device_ = interface_->createDevice();
  if (!device_) {
    if (error) *error = "pas de peripherique Vulkan";
    return false;
  }
  device_name_ = device_->getDescription().name;
  if (!renderer_.Initialize(device_.get(), physical, std::move(container), &shaders_, watcher, profile)) {
    if (error) *error = "initialisation du moteur de rendu";
    return false;
  }
  return true;
}

}  // namespace native_gpu

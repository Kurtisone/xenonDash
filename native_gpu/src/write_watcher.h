// SPDX-License-Identifier: GPL-3.0-or-later
// native_gpu : surveillance des ecritures de la memoire physique invitee (une page de 4 Ko = un cachet de sequence), pour que le noyau
// ne reconvertisse que les textures et tampons modifies. Meme code que PhysicalWriteWatcher de front6770.cpp (reprise de
// live_renderer.cpp de Skate1Native, commit 81805f51) ; front6770.cpp garde sa copie, a unifier.
#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>

#include <rex/system/kernel_state.h>

#include "renderer.h"

namespace ngpu {

class PhysicalWriteWatcher : public native_gpu::WriteWatcher {
 public:
  explicit PhysicalWriteWatcher(rex::memory::Memory* memory) : memory_(memory) {
    handle_ = memory_->RegisterPhysicalMemoryInvalidationCallback(&OnWrite, this);
  }
  ~PhysicalWriteWatcher() override { memory_->UnregisterPhysicalMemoryInvalidationCallback(handle_); }
  uint32_t Watch(uint32_t address, uint32_t size) override {
    const uint32_t stamp = sequence_.load(std::memory_order_acquire);
    memory_->EnablePhysicalMemoryAccessCallbacks(address, size, true, false);
    return stamp;
  }
  bool Dirty(uint32_t address, uint32_t size, uint32_t stamp) override {
    if (!size) return false;
    const uint32_t first = (address & 0x1FFFFFFF) >> 12;
    const uint32_t last = std::min(kPages - 1, ((address & 0x1FFFFFFF) + size - 1) >> 12);
    for (uint32_t page = first; page <= last;) {
      if (blocks_[page >> 6].load(std::memory_order_relaxed) <= stamp) {
        page = (page | 63) + 1;
        continue;
      }
      if (pages_[page].load(std::memory_order_relaxed) > stamp) return true;
      ++page;
    }
    return false;
  }

 private:
  static constexpr uint32_t kPages = 0x20000000 >> 12;
  static std::pair<uint32_t, uint32_t> OnWrite(void* context, uint32_t start, uint32_t length, bool) {
    auto* self = static_cast<PhysicalWriteWatcher*>(context);
    if (!length) return {start, 0};
    const uint32_t sequence = self->sequence_.fetch_add(1, std::memory_order_acq_rel) + 1;
    const uint32_t first = (start & 0x1FFFFFFF) >> 12;
    const uint32_t last = std::min(kPages - 1, ((start & 0x1FFFFFFF) + length - 1) >> 12);
    for (uint32_t page = first; page <= last; ++page) {
      self->pages_[page].store(sequence, std::memory_order_relaxed);
      self->blocks_[page >> 6].store(sequence, std::memory_order_relaxed);
    }
    return {start, length};
  }
  rex::memory::Memory* memory_;
  void* handle_ = nullptr;
  std::atomic<uint32_t> sequence_{1};
  std::array<std::atomic<uint32_t>, kPages> pages_{};
  std::array<std::atomic<uint32_t>, kPages / 64> blocks_{};
};

}  // namespace ngpu

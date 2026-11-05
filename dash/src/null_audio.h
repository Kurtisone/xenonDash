// dash - backend audio "silencieux" pour machines sans carte son.
//
// Le NopAudioSystem du SDK refuse de créer un pilote (X_STATUS_NOT_IMPLEMENTED), ce
// qui fait échouer XAudioRegisterRenderDriverClient : le Dashboard désenregistre alors
// la classe XUI XuiSoundXAudio et aucune scène ne se charge.
//
// Ici, le pilote accepte les trames et les jette, en libérant le sémaphore du client
// au rythme d'une vraie sortie (256 échantillons à 48 kHz = 5,33 ms par trame), pour
// que le moteur audio invité soit cadencé normalement.

#pragma once

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>

#include <rex/audio/audio_driver.h>
#include <rex/audio/audio_system.h>
#include <rex/thread.h>

namespace dash::audio {

using rex::X_STATUS;  // requis par la macro X_STATUS_SUCCESS

class NullAudioDriver final : public rex::audio::AudioDriver {
 public:
  NullAudioDriver(rex::memory::Memory* memory, rex::thread::Semaphore* semaphore)
      : AudioDriver(memory), semaphore_(semaphore), worker_([this] { Run(); }) {}

  ~NullAudioDriver() override {
    running_ = false;
    if (worker_.joinable()) worker_.join();
  }

  void SubmitFrame(uint32_t /*samples_ptr*/) override { pending_.fetch_add(1); }

 private:
  static constexpr auto kFramePeriod = std::chrono::microseconds(256 * 1000000 / 48000);

  void Run() {
    auto next = std::chrono::steady_clock::now();
    while (running_) {
      next += kFramePeriod;
      std::this_thread::sleep_until(next);
      // Une trame "jouée" : on rend une place au client, comme le ferait la carte son.
      uint32_t p = pending_.load();
      while (p > 0 && !pending_.compare_exchange_weak(p, p - 1)) {
      }
      if (p > 0) semaphore_->Release(1, nullptr);
    }
  }

  rex::thread::Semaphore* semaphore_;
  std::atomic<uint32_t> pending_{0};
  std::atomic<bool> running_{true};
  std::thread worker_;
};

class NullAudioSystem final : public rex::audio::AudioSystem {
 public:
  explicit NullAudioSystem(rex::runtime::FunctionDispatcher* fd) : AudioSystem(fd) {}

  static std::unique_ptr<rex::audio::AudioSystem> Create(rex::runtime::FunctionDispatcher* fd) {
    return std::make_unique<NullAudioSystem>(fd);
  }

 protected:
  rex::X_STATUS CreateDriver(size_t /*index*/, rex::thread::Semaphore* semaphore,
                        rex::audio::AudioDriver** out_driver) override {
    *out_driver = new NullAudioDriver(memory(), semaphore);
    return X_STATUS_SUCCESS;
  }

  void DestroyDriver(rex::audio::AudioDriver* driver) override { delete driver; }
};

}  // namespace dash::audio

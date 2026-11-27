// xenondash common - manette virtuelle pour la couche d'entrée de XAM recompilé (palier H).
//
// Sur la console, le pilote USB du noyau signale l'arrivée d'une manette à XAM : XAM enregistre un rappel par
// DrvSetUserBindingCallback (sub_818AD898 : (r3 = poignée du pilote, r4 = identifiant du périphérique, r5 = type, r6 = retrait,
// r7 = &port)), le noyau l'appelle, XAM range le périphérique dans son tableau (0x81BFD948 : 4 ports de 56 octets, 7 types
// de 8 octets {présent, identifiant}) puis lit la manette par XInputdGetCapabilities / XInputdReadState et signale le port par
// XInputdNotify. Sans arrivée le tableau reste vide et XamInputGetKeystrokeHud renvoie toujours ERROR_EMPTY.
// Ici : le rappel enregistré est appelé une fois par manette (type 0) quand XAM a posé son propre rappel d'arrivée (mot invité
// 0x81BE1E10), et les trois fonctions XInputd lisent le même InputSystem que XamInputGetState du Dashboard (clavier/manette réels).
// Une manette virtuelle PAR EMPLACEMENT de l'InputSystem (SlotAssignment : ordinal N = utilisateur N) : l'identifiant
// 0x10000001 + N lit GetState(N). L'emplacement 0 (clavier et première manette) est annoncé au démarrage, comme avant ;
// les emplacements 1 à 3 le sont à la connexion d'une manette et retirés à sa déconnexion (r6 = 1), sur le port N de XAM.
// Sans cela, XAM ne voyait que le port 0 : le Guide, le clavier virtuel et les boîtes ignoraient toute autre manette.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <thread>

#include <rex/hook.h>
#include <rex/input/input.h>
#include <rex/input/input_system.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/function.h>
#include <rex/runtime.h>
#include <rex/system/function_dispatcher.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xthread.h>

#include "kernel_sdk_fn.h"

namespace {

constexpr uint32_t kDeviceId = 0x10000001u;          // identifiant de la manette virtuelle de l'emplacement 0 (opaque pour XAM)
constexpr uint32_t kMaxPads = 4;                      // emplacements (utilisateurs) de l'InputSystem, ports de XAM
constexpr uint32_t kStatusNotConnected = 0xC000009Du;  // STATUS_DEVICE_NOT_CONNECTED
constexpr uint32_t kXamArrivalSlot = 0x81BE1E10u;     // rappel d'arrivée de XAM pour le type 0 (posé à l'initialisation)

std::atomic<uint32_t> g_binding_cb{0};
std::atomic<bool> g_arrival_started{false};
std::atomic<uint32_t> g_pad_mask{0};  // emplacements annoncés à XAM (bit N = manette N), lu par xam_input_patch.cpp

// Emplacement servi par un identifiant de manette virtuelle, -1 s'il n'en est pas un.
int SlotOfId(uint32_t id) { return id >= kDeviceId && id < kDeviceId + kMaxPads ? int(id - kDeviceId) : -1; }

rex::input::InputSystem* Input() {
  auto* ks = REX_KERNEL_STATE();
  return ks && ks->emulator() ? static_cast<rex::input::InputSystem*>(ks->emulator()->input_system()) : nullptr;
}

uint32_t LoadBe(uint8_t* base, uint32_t ea) {
  return __builtin_bswap32(*reinterpret_cast<uint32_t*>(base + ea));
}

// Appelle le rappel de liaison de XAM depuis un fil invité : arrivée ou retrait (removal) de la manette de l'emplacement `slot`.
// Emplacement 0 : port quelconque (appel d'origine). Autres : port imposé = emplacement, pour que XamInputGetState(N) de XAM et du
// Dashboard (utilisateur N de l'InputSystem) désignent la même manette.
void RunBinding(rex::system::KernelState* ks, uint32_t cb, int slot, bool removal) {
  auto* ts = rex::runtime::ThreadState::Get();
  if (!ts) return;
  uint8_t* base = ks->memory()->virtual_membase();
  PPCFunc* fn = ks->function_dispatcher()->GetFunction(cb);
  if (!fn) {
    REXLOG_WARN("[xinputd] rappel de liaison 0x{:08X} : fonction absente", cb);
    return;
  }
  static const uint32_t port = ks->memory()->SystemHeapAlloc(4);  // un seul fil appelle cette fonction
  base[port] = slot == 0 ? 0xFF : uint8_t(slot);
  const uint32_t id = kDeviceId + uint32_t(slot);
  PPCContext ctx = *ts->context();
  ctx.r1.u64 = static_cast<uint32_t>(ctx.r1.u64) - 0x100;
  ctx.r3.u64 = id;       // poignée du pilote (distincte par manette : XAM range le port par poignée)
  ctx.r4.u64 = id;       // identifiant du périphérique
  ctx.r5.u64 = 0;        // type 0 : manette
  ctx.r6.u64 = removal;  // 0 : arrivée, 1 : retrait
  ctx.r7.u64 = port;
  fn(ctx, base);
  if (removal) {
    g_pad_mask.fetch_and(~(1u << slot));
  } else {
    g_pad_mask.fetch_or(1u << slot);
  }
  REXLOG_INFO("[xinputd] manette virtuelle 0x{:08X} (emplacement {}) {} à XAM : retour r3=0x{:08X}, port={}", id, slot,
              removal ? "retirée" : "annoncée", ctx.r3.u32, base[port]);
}

// Emplacement connecté ? L'emplacement 0 l'est toujours (clavier du pilote MnK, comme avant) ; les autres, quand l'InputSystem leur
// assigne une manette (GetCapabilities ne touche ni aux fronts ni aux compteurs de paquets).
bool SlotConnected(int slot) {
  if (slot == 0) return true;
  auto* is = Input();
  rex::input::X_INPUT_CAPABILITIES caps{};
  return is && is->GetCapabilities(uint32_t(slot), 0, &caps) == 0;
}

// Corps du fil invité : annonce l'emplacement 0, puis suit la connexion des emplacements 1 à 3 (relevé toutes les 250 ms ; un
// retrait n'est annoncé qu'après 1 s d'absence continue, pour ne pas prendre un battement de SDL pour un débranchement).
void RunBindings(rex::system::KernelState* ks, uint32_t cb) {
  RunBinding(ks, cb, 0, false);
  bool announced[kMaxPads] = {true, false, false, false};
  int absent[kMaxPads] = {};
  for (;;) {
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    for (int slot = 1; slot < int(kMaxPads); ++slot) {
      const bool on = SlotConnected(slot);
      absent[slot] = on ? 0 : absent[slot] + 1;
      if (on && !announced[slot]) {
        RunBinding(ks, cb, slot, false);
        announced[slot] = true;
      } else if (!on && announced[slot] && absent[slot] >= 4) {
        RunBinding(ks, cb, slot, true);
        announced[slot] = false;
      }
    }
  }
}

void StartArrival(uint32_t cb) {
  if (g_arrival_started.exchange(true)) return;
  auto* ks = REX_KERNEL_STATE();
  std::thread([ks, cb] {
    uint8_t* base = ks->memory()->virtual_membase();
    int waited_ms = 0;
    const int timeout_ms = 30000;
    while (LoadBe(base, kXamArrivalSlot) == 0 && waited_ms < timeout_ms) {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      waited_ms += 50;
    }
    if (LoadBe(base, kXamArrivalSlot) == 0) {
      REXLOG_WARN("[xinputd] rappel d'arrivée de XAM non posé après {} ms : arrivée émise quand même", timeout_ms);
    } else {
      std::this_thread::sleep_for(std::chrono::milliseconds(300));
      REXLOG_INFO("[xinputd] rappel d'arrivée de XAM posé après {} ms", waited_ms);
    }
    static rex::system::object_ref<rex::system::XHostThread> host;  // vit jusqu'à la fin du processus
    host = rex::system::object_ref<rex::system::XHostThread>(
        new rex::system::XHostThread(ks, 256 * 1024, 0, [ks, cb] {
          RunBindings(ks, cb);
          return 0;
        }));
    host->set_name("XInput arrival");
    host->Create();
  }).detach();
}

}  // namespace

// Emplacements de manette annoncés à XAM (bit N = manette N) : lu par le correctif d'entrée du module XAM (src_xam_patch).
extern "C" uint32_t dash_xinputd_pad_mask() { return g_pad_mask.load(); }

// DrvSetUserBindingCallback(callback) : mémorisé ; l'arrivée de la manette virtuelle est lancée.
REX_HOOK_RAW(__imp__DrvSetUserBindingCallback) {
  static PPCFunc* fn = dash::SdkFunction("__imp__DrvSetUserBindingCallback");
  if (fn) fn(ctx, base);
  const uint32_t cb = ctx.r3.u32;
  REXLOG_INFO("[xinputd] DrvSetUserBindingCallback(0x{:08X})", cb);
  if (cb) {
    g_binding_cb.store(cb);
    StartArrival(cb);
  }
}

// XInputdGetCapabilities(identifiant, XINPUT_CAPABILITIES*) : manette standard (valeurs de XInputGetCapabilities).
REX_HOOK_RAW(__imp__XInputdGetCapabilities) {
  const uint32_t id = ctx.r3.u32, out = ctx.r4.u32;
  const int slot = SlotOfId(id);
  if (slot < 0 || !out || !SlotConnected(slot)) {
    ctx.r3.u64 = kStatusNotConnected;
    return;
  }
  auto* caps = reinterpret_cast<rex::input::X_INPUT_CAPABILITIES*>(base + out);
  std::memset(caps, 0, sizeof(*caps));
  caps->type = rex::input::XINPUT_DEVTYPE_GAMEPAD;
  caps->sub_type = rex::input::XINPUT_DEVSUBTYPE_GAMEPAD;
  caps->flags = 0;
  caps->gamepad.buttons = 0xF3FF;
  caps->gamepad.left_trigger = 0xFF;
  caps->gamepad.right_trigger = 0xFF;
  caps->gamepad.thumb_lx = static_cast<int16_t>(0xFFC0);
  caps->gamepad.thumb_ly = static_cast<int16_t>(0xFFC0);
  caps->gamepad.thumb_rx = static_cast<int16_t>(0xFFC0);
  caps->gamepad.thumb_ry = static_cast<int16_t>(0xFFC0);
  caps->vibration.left_motor_speed = 0xFF00;
  caps->vibration.right_motor_speed = 0xFF00;
  static std::atomic<int> n{0};
  if (n.fetch_add(1) < 3) REXLOG_INFO("[xinputd] XInputdGetCapabilities(0x{:08X}) : manette standard", id);
  ctx.r3.u64 = 0;
}

// XInputdReadState(identifiant, uint32_t* numéroDePaquet, XINPUT_GAMEPAD*) : état lu dans l'InputSystem (utilisateur = emplacement).
REX_HOOK_RAW(__imp__XInputdReadState) {
  const uint32_t id = ctx.r3.u32, packet = ctx.r4.u32, pad = ctx.r5.u32;
  auto* is = Input();
  const int slot = SlotOfId(id);
  if (slot < 0 || !is) {
    ctx.r3.u64 = kStatusNotConnected;
    return;
  }
  rex::input::X_INPUT_STATE st{};
  if (is->GetState(uint32_t(slot), &st) != 0) {
    // Diagnostic manette (2026-11-21) : XAM interroge mais l'InputSystem n'a pas d'état — distinguer
    // ce cas d'un XAM qui ne pollé pas du tout (session du 14:20 sans aucune trace de lecture).
    static std::atomic<int> n{0};
    if (n.fetch_add(1) < 5) REXLOG_WARN("[xinputd] XInputdReadState : l'InputSystem n'a pas d'etat (emplacement {})", slot);
    ctx.r3.u64 = kStatusNotConnected;
    return;
  }
  if (packet) *reinterpret_cast<uint32_t*>(base + packet) = __builtin_bswap32(static_cast<uint32_t>(st.packet_number));
  if (pad) std::memcpy(base + pad, &st.gamepad, sizeof(st.gamepad));
  static std::atomic<uint16_t> last[kMaxPads] = {};
  const uint16_t b = static_cast<uint16_t>(st.gamepad.buttons);
  if (b != last[slot].exchange(b)) REXLOG_INFO("[xinputd] XInputdReadState(emplacement {}) : boutons 0x{:04X}", slot, b);
  ctx.r3.u64 = 0;
}

// XInputdNotify(identifiant, port | 0x90000000) : XAM annonce l'attribution du port ; rien à faire.
REX_HOOK_RAW(__imp__XInputdNotify) {
  static std::atomic<int> n{0};
  if (n.fetch_add(1) < 5)
    REXLOG_INFO("[xinputd] XInputdNotify(0x{:08X}, 0x{:08X})", ctx.r3.u32, ctx.r4.u32);
  ctx.r3.u64 = 0;
}

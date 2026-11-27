// xenondash common - manettes SDL virtuelles de test (banc sans manette réelle).
//
// DASH_TEST_PAD_FIFO=<chemin> : crée un tube nommé lu ligne à ligne par un fil hôte. Commandes :
//   attach <n>              branche la manette virtuelle <n> (0 à 3) : « Test Pad <n> », reconnue par le pilote SDL du SDK comme
//                           une vraie manette (ordinal libre le plus bas = utilisateur de l'InputSystem = port de XAM)
//   detach <n>              la débranche
//   <n> <bouton> 1|0        appuie / relâche : a b x y back guide start ls rs lb rb up down left right
//   <n> axis <lx|ly|rx|ry|lt|rt> <valeur>   axe SDL (-32768 à 32767)
// Sans manette réelle ni uinput, c'est la seule façon d'exercer l'attribution des manettes (slots) d'une session. Sans la
// variable, aucun effet. SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS=1 évite que SDL ignore les événements d'une fenêtre sans focus.

#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>

#include <SDL3/SDL.h>

#include <rex/logging.h>

namespace dash::testpads {

namespace {

constexpr int kPads = 4;
SDL_JoystickID g_ids[kPads] = {};
SDL_Joystick* g_joy[kPads] = {};

int ButtonIndex(const std::string& n) {
  static const char* names[] = {"a", "b", "x", "y", "back", "guide", "start", "ls", "rs", "lb", "rb", "up", "down", "left", "right"};
  for (int i = 0; i < 15; ++i)
    if (n == names[i]) return i;
  return -1;
}

int AxisIndex(const std::string& n) {
  static const char* names[] = {"lx", "ly", "rx", "ry", "lt", "rt"};
  for (int i = 0; i < 6; ++i)
    if (n == names[i]) return i;
  return -1;
}

void Attach(int n) {
  if (n < 0 || n >= kPads || g_joy[n]) return;
  static std::string names[kPads];
  names[n] = "Test Pad " + std::to_string(n);
  SDL_VirtualJoystickDesc desc;
  SDL_INIT_INTERFACE(&desc);
  desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
  desc.naxes = 6;
  desc.nbuttons = 15;
  desc.button_mask = (1u << 15) - 1;
  desc.axis_mask = (1u << 6) - 1;
  desc.name = names[n].c_str();
  const SDL_JoystickID id = SDL_AttachVirtualJoystick(&desc);
  if (!id) {
    REXLOG_WARN("[testpad] attach {} : {}", n, SDL_GetError());
    return;
  }
  g_ids[n] = id;
  g_joy[n] = SDL_OpenJoystick(id);
  REXLOG_INFO("[testpad] manette virtuelle {} branchée (id SDL {})", n, uint32_t(id));
}

void Detach(int n) {
  if (n < 0 || n >= kPads || !g_joy[n]) return;
  SDL_CloseJoystick(g_joy[n]);
  SDL_DetachVirtualJoystick(g_ids[n]);
  g_joy[n] = nullptr;
  REXLOG_INFO("[testpad] manette virtuelle {} débranchée", n);
}

void Loop(std::string path) {
  // Le sous-système joystick est initialisé par le pilote SDL du SDK (fenêtre créée) : attendre qu'il le soit.
  for (int i = 0; i < 600 && !SDL_WasInit(SDL_INIT_GAMEPAD); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
  ::unlink(path.c_str());
  if (::mkfifo(path.c_str(), 0600) != 0) {
    REXLOG_WARN("[testpad] mkfifo {} a échoué", path);
    return;
  }
  REXLOG_INFO("[testpad] commandes lues dans {}", path);
  for (;;) {
    std::ifstream in(path);  // l'ouverture bloque jusqu'à ce qu'un écrivain se présente
    std::string line;
    while (std::getline(in, line)) {
      char w1[32] = {}, w2[32] = {}, w3[32] = {};
      int n = -1, v = 0;
      if (std::sscanf(line.c_str(), "attach %d", &n) == 1) {
        Attach(n);
      } else if (std::sscanf(line.c_str(), "detach %d", &n) == 1) {
        Detach(n);
      } else if (std::sscanf(line.c_str(), "%d %31s %31s %d", &n, w1, w2, &v) == 4 && !std::strcmp(w1, "axis")) {
        const int a = AxisIndex(w2);
        if (n >= 0 && n < kPads && g_joy[n] && a >= 0) SDL_SetJoystickVirtualAxis(g_joy[n], a, Sint16(v));
      } else if (std::sscanf(line.c_str(), "%d %31s %d", &n, w1, &v) == 3) {
        const int b = ButtonIndex(w1);
        if (n >= 0 && n < kPads && g_joy[n] && b >= 0) SDL_SetJoystickVirtualButton(g_joy[n], b, v != 0);
      } else {
        REXLOG_WARN("[testpad] commande inconnue : {}", line);
      }
      (void)w3;
    }
  }
}

}  // namespace

void Init() {
  const char* path = std::getenv("DASH_TEST_PAD_FIFO");
  if (!path || !*path) return;
  std::thread(Loop, std::string(path)).detach();
}

}  // namespace dash::testpads

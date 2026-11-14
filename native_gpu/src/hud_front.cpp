// SPDX-License-Identifier: GPL-3.0-or-later
// native_gpu : facade D3D du HUD de XAM 13604 (boites de message, clavier virtuel, connexion, creation de profil) sur le noyau de rendu
// commun ngpu_core. Aucun plugin GPU, aucun lecteur de tampon de commandes PM4 : les fonctions D3D (D3D9 2.0.2099, XDK bien plus
// ancienne que celle du Dashboard) liees dans xam.xex et recompilees avec lui sont crochetees (src_xam_d3d/xam_d3d_hooks.cpp : une
// definition forte de chaque sub_<adresse> appelle hudd3d_call, ci-dessous) ; le noyau rend l'etat Xenos que le D3D invite a deja
// ecrit dans son peripherique. Le HUD a son propre peripherique D3D (autre adresse que celui du Dashboard) et son propre noyau :
// l'image 852x480 qu'il resout est relue (RGBA8, alpha compris) et superposee a l'image du Dashboard a la presentation
// (HudComposite, appelee par front_profile.cpp).
//
// Meme principe que front_profile.cpp (Dashboard 13604) : l'ombre du peripherique est un instantane canonique du peripherique
// (xenos::Canonicalize), le microcode du vertex shader est celui que D3D a patche (vfetch), le dessin ouvert par BeginVertices est
// clos au prochain appel suivi. Differences : D3D 2099 (groupes de registres decales, objets shader et conteneurs de l'ancien
// format 0x102A0E0x convertis par tools/hud_shaders.py), un seul peripherique, pas de GPU nul a fournir (le HUD n'attend rien du
// GPU), cadence limitee a 60 Hz a la soumission (la boucle d'image du HUD tourne sinon a ~2400 images/s, faute de vsync).

#include <sys/stat.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#define XXH_INLINE_ALL
#include <xxhash.h>

#include <rex/system/kernel_state.h>

#include "device.h"
#include "device_layout.h"
#include "ngpu.h"
#include "write_watcher.h"
#include "hud_xam13604.h"

namespace {

using namespace ngpu;
namespace ng = native_gpu;

struct HudObject {
  bool pixel = false;
  uint64_t key = 0;
  uint32_t ucode = 0, ucode_size = 0;
  std::vector<std::pair<uint32_t, uint32_t>> vfetch;  // vertex shader : (adresse d'instruction, usage) des elements d'entree
};

struct InlineDraw {
  bool open = false;
  uint32_t vertices = 0, primitive = 0, count = 0, stride = 0, vs = 0, ps = 0;
  std::vector<uint8_t> state, vs_physical;
};

struct Hud {
  std::mutex mutex;
  bool init_tried = false;
  std::unique_ptr<ngpu::PhysicalWriteWatcher> watcher;
  std::unique_ptr<ng::Core> core;
  bool in_frame = false;
  uint64_t frames = 0, draws = 0, skipped = 0, clears = 0, resolves = 0;
  uint32_t dest = 0;  // destination du dernier Resolve (surface de l'overlay)
  // Image au repos (overlay vide a l'image precedente) : une image sur quatre seulement est rendue (le HUD tourne a 60 Hz sans rien montrer, la
  // boite apparait au plus 50 ms plus tard).
  bool skipping = false;
  uint32_t idle_phase = 0;
  float frame_dim = 1.0f;  // facteur du fond atténué demandé par cette image (voir ExecuteDraw), 1 = aucun
  bool need_clear = true;  // l'overlay est redessine entierement a chaque image : la cible est remise a zero (transparent) avant le premier dessin
  std::unordered_map<uint32_t, HudObject> objects;
  std::unordered_map<uint64_t, std::vector<uint8_t>> containers;  // cle -> conteneur converti (partie virtuelle puis physique)
  std::map<std::string, int> missing;
  InlineDraw inl;
  alignas(16) uint8_t shadow[xenos::kCanonicalStateSize] = {};
  std::vector<uint8_t> scratch, vs_copy;
  std::chrono::steady_clock::time_point next_frame{};
} g;

// ---- l'image superposee (RGBA8 avec alpha), publiee par le fil du HUD, lue par le fil de presentation du Dashboard ----
struct Overlay {
  std::mutex mutex;
  std::vector<uint8_t> rgba;
  uint32_t w = 0, h = 0;
  bool visible = false;  // au moins quelques pixels d'alpha notable
  float dim = 1.0f;      // facteur applique a l'image du Dashboard sous l'overlay (fond atténué), 1 = aucun
  uint64_t seq = 0;
} g_overlay;

// DASH_HUD_EVLOG=<n> : detail des dessins de l'image n (n = 0 : les 60 premiers dessins).
bool Evlog() {
  static const long frame = std::getenv("DASH_HUD_EVLOG") ? std::atol(std::getenv("DASH_HUD_EVLOG")) : -1;
  return frame >= 0 && (frame == 0 ? g.draws <= 60 : long(g.frames) == frame);
}

const xenos::DeviceLayout& Layout() {
  static const xenos::DeviceLayout layout = [] {
    xenos::DeviceLayout l;
    for (auto& r : l.register_groups) r += hud_profile::kRegisterShift;
    l.index_buffer = hud_profile::kIndexBuffer;
    l.pixel_shader = hud_profile::kPixelShader;
    l.vertex_shader = hud_profile::kVertexShader;
    return l;
  }();
  return layout;
}

// Dossier des <cle>.spv / <cle>.cont du HUD : DASH_HUD_SHADERS, sinon <game_data_root>/shaders/hud (fixé par dash_app_base.cpp).
std::string ShaderDir() {
  const char* e = std::getenv("DASH_HUD_SHADERS");
  return e && *e ? e : "shaders/hud";
}
// Un dossier absent donne un seul message (lancer la régénération) ; un dossier présent mais incomplet, une ligne par shader.
void ReportMissingShader(const char* name, bool pixel, uint32_t object) {
  static bool dir_reported = false;
  const std::string dir = ShaderDir();
  struct stat st;
  if (::stat(dir.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) {
    if (!dir_reported) {
      dir_reported = true;
      NGPU_ERR("shaders natifs du HUD introuvables dans %s : lancer la régénération (XenonDash --regen-shaders)", dir.c_str());
    }
    return;
  }
  NGPU_ERR("HUD : shader %s %s (objet %08X) introuvable dans %s", pixel ? "PS" : "VS", name, object, dir.c_str());
}

bool ReadFile(const std::string& path, std::vector<uint8_t>& out) {
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) return false;
  std::fseek(f, 0, SEEK_END);
  out.resize(size_t(std::max(std::ftell(f), 0L)));
  std::fseek(f, 0, SEEK_SET);
  out.resize(std::fread(out.data(), 1, out.size(), f));
  std::fclose(f);
  return true;
}

// Appelee avec g.mutex tenu.
bool EnsureCore() {
  if (g.core) return true;
  if (g.init_tried) return false;
  g.init_tried = true;
  auto* memory = rex::system::kernel_memory();
  g.watcher = std::make_unique<ngpu::PhysicalWriteWatcher>(memory);
  auto core = std::make_unique<ng::Core>();
  ng::Profile profile;  // aucune echelle de scene ni liste d'interface
  profile.scene_pitch_min = 1u << 30;
  profile.scene_pitch_max = 0;
  std::string error;
  if (!core->Initialize(
          memory->TranslatePhysical(0),
          [](uint64_t key) -> const uint8_t* {
            auto it = g.containers.find(key);
            return it == g.containers.end() ? nullptr : it->second.data();
          },
          g.watcher.get(), profile, &error)) {
    NGPU_ERR("HUD : noyau de rendu : %s", error.c_str());
    return false;
  }
  std::fprintf(stderr, "[hud] noyau ngpu_core sur '%s' (HUD de XAM, D3D 2099)\n", core->device_name().c_str());
  g.core = std::move(core);
  return true;
}

void BeginFrameIfNeeded() {
  if (!g.in_frame) {
    g.core->renderer().BeginFrame();
    g.in_frame = true;
  }
}

// ---- ombre canonique : instantane du peripherique, adresses de textures de la region E corrigees ----
void Snapshot(const uint8_t* base, uint32_t dev, uint8_t* out) {
  xenos::Canonicalize(Layout(), base + dev, out);
  for (uint32_t slot = 0; slot < 32; ++slot) {
    uint8_t* fetch = out + xenos::device::kFetchConstants + 24 * slot;
    uint32_t d[6];
    for (int i = 0; i < 6; ++i) d[i] = Be32(fetch + 4 * i);
    if ((d[0] & 3) != 2) continue;  // pas une texture
    const auto fix = [](uint32_t word) {
      const uint32_t addr = word & 0xFFFFF000;
      return addr >= 0xE0000000u ? (word & 0xFFF) | (addr + 0x1000) : word;
    };
    d[1] = fix(d[1]);
    if (d[5] & 0xFFFFF000) d[5] = fix(d[5]);
    for (int i = 0; i < 6; ++i) PutBe32(fetch + 4 * i, d[i]);
  }
}

// ---- objets shader : conteneur (ancien format) copie dans l'objet, pointeur du microcode dans l'objet ----
// Cle = XXH3-64 du microcode, instructions vfetch des vertex shaders mises a zero (D3D les remplit) : voir tools/hud_shaders.py.
const HudObject* RegisterObject(const uint8_t* base, uint32_t object, bool pixel) {
  auto it = g.objects.find(object);
  if (it != g.objects.end() && it->second.pixel == pixel) return &it->second;
  const uint8_t* c = base + object + (pixel ? hud_profile::kPsContainer : hud_profile::kVsContainer);
  if ((Be32(c) & 0xFFFFFFFE) != 0x102A0E00 || (Be32(c) & 1) == uint32_t(pixel)) return nullptr;  // 0x102A0E01 = vertex, 0x102A0E00 = pixel
  const uint32_t vsize = Be32(c + 4), psize = Be32(c + 8), shader_offset = Be32(c + 0x14);
  const uint32_t ucode = Be32(base + object + (pixel ? hud_profile::kPsMicrocodePtr : hud_profile::kVsMicrocodePtr));
  if (vsize < 0x20 || vsize > 0x1000 || !psize || psize > 0x4000 || shader_offset < 0x18 || shader_offset >= vsize || !ucode) return nullptr;
  HudObject info;
  info.pixel = pixel;
  info.ucode = ucode;
  info.ucode_size = psize;
  std::vector<uint8_t> micro(base + ucode, base + ucode + psize);
  if (!pixel) {
    // Ancienne structure du shader : {field8, fieldC, field10, 0, interpolatorInfo, ?, field18, nb d'elements, field20, 0, tableau...}.
    const uint32_t nwords = (vsize - shader_offset) / 4;
    auto word = [&](uint32_t i) { return i < nwords ? Be32(c + shader_offset + 4 * i) : 0u; };
    const uint32_t field18 = word(6), count = word(7);
    for (uint32_t i = 0; i < count && i < 16; ++i) {
      const uint32_t v = word(10 + field18 + i);
      info.vfetch.push_back({v & 0xFFF, (v >> 12) & 0xF});
      if ((v & 0xFFF) * 12 + 12 <= micro.size()) std::memset(micro.data() + (v & 0xFFF) * 12, 0, 12);
    }
  }
  info.key = XXH3_64bits(micro.data(), micro.size());
  if (!g.containers.count(info.key)) {
    char name[48];
    std::snprintf(name, sizeof name, "%016llX", (unsigned long long)info.key);
    std::vector<uint8_t> container;
    const std::string spv = ShaderDir() + "/" + name + ".spv";
    if (EnsureCore() && ReadFile(ShaderDir() + "/" + name + ".cont", container) && !container.empty()) {
      g.core->shaders().AddFile(info.key, spv);
      g.containers.emplace(info.key, std::move(container));
    } else if (g.missing[name]++ == 0) {
      ReportMissingShader(name, pixel, object);
    }
  }
  return &(g.objects[object] = std::move(info));
}

// ---- elements de sommets : lus dans les vfetch du microcode patche (format, decalage) ----
struct Elem {
  uint32_t offset, format;
};
size_t ElementBytes(uint32_t format) {
  switch (format) {
    case 36: case 6: case 25: case 31: return 4;
    case 37: case 26: case 32: return 8;
    case 57: return 12;
    case 38: return 16;
    default: return 4;
  }
}
std::vector<Elem> Elements(const HudObject& vs, const uint8_t* physical, uint32_t physical_size) {
  std::vector<Elem> out;
  for (const auto& [address, usage] : vs.vfetch) {
    const uint32_t at = address * 12;
    if (at + 12 > physical_size) continue;
    const uint32_t w1 = Be32(physical + at + 4), w2 = Be32(physical + at + 8);
    out.push_back({((w2 >> 8) & 0x7FFFFF) * 4, (w1 >> 16) & 0x3F});
  }
  return out;
}

// Developpe rect list (8), fan (5) et quads (13, sans indices) en liste de triangles : le quatrieme sommet d'un rectangle = b + c - a.
void ExpandPrimitive(uint32_t prim, uint32_t count, const uint8_t* data, uint32_t stride, const std::vector<Elem>& elems, std::vector<uint8_t>& out,
                     uint32_t& out_count) {
  auto vertex = [&](uint32_t i) { return data + size_t(i) * stride; };
  out.clear();
  out_count = 0;
  auto emit = [&](const uint8_t* v) {
    out.insert(out.end(), v, v + stride);
    ++out_count;
  };
  std::vector<uint8_t> fourth(stride);
  auto combine = [&](const uint8_t* a, const uint8_t* b, const uint8_t* c) {
    std::memcpy(fourth.data(), c, stride);
    for (const Elem& e : elems) {
      if (e.offset + ElementBytes(e.format) > stride) continue;
      uint8_t* d = fourth.data() + e.offset;
      switch (e.format) {
        case 36: case 37: case 57: case 38: {
          const uint32_t n = e.format == 36 ? 1 : e.format == 37 ? 2 : e.format == 57 ? 3 : 4;
          for (uint32_t k = 0; k < n; ++k) {
            const float r = BeF32(b + e.offset + 4 * k) + BeF32(c + e.offset + 4 * k) - BeF32(a + e.offset + 4 * k);
            uint32_t bits;
            std::memcpy(&bits, &r, 4);
            PutBe32(d + 4 * k, bits);
          }
          break;
        }
        case 6:
          for (int k = 0; k < 4; ++k) d[k] = uint8_t(std::clamp(int(b[e.offset + k]) + c[e.offset + k] - a[e.offset + k], 0, 255));
          break;
        case 25: case 26: {
          const uint32_t n = e.format == 25 ? 2 : 4;
          for (uint32_t k = 0; k < n; ++k) {
            const int v = int(int16_t(Be16(b + e.offset + 2 * k))) + int16_t(Be16(c + e.offset + 2 * k)) - int16_t(Be16(a + e.offset + 2 * k));
            const uint16_t u = uint16_t(int16_t(std::clamp(v, -32768, 32767)));
            d[2 * k] = u >> 8;
            d[2 * k + 1] = u & 0xFF;
          }
          break;
        }
        default:
          std::memcpy(d, a + e.offset, ElementBytes(e.format));
          break;
      }
    }
    return fourth.data();
  };
  if (prim == 8) {
    for (uint32_t i = 0; i + 2 < count; i += 3) {
      const uint8_t *a = vertex(i), *b = vertex(i + 1), *c = vertex(i + 2);
      std::vector<uint8_t> dd(combine(a, b, c), combine(a, b, c) + stride);
      emit(a); emit(b); emit(c); emit(c); emit(b); emit(dd.data());
    }
  } else if (prim == 5) {
    for (uint32_t i = 1; i + 1 < count; ++i) {
      emit(vertex(0)); emit(vertex(i)); emit(vertex(i + 1));
    }
  } else if (prim == 13) {
    for (uint32_t i = 0; i + 3 < count; i += 4) {
      emit(vertex(i)); emit(vertex(i + 1)); emit(vertex(i + 2)); emit(vertex(i)); emit(vertex(i + 2)); emit(vertex(i + 3));
    }
  }
}

// ---- dessin ----
// Appelee avec g.mutex tenu. `state` : ombre figee au BeginVertices ; `vs_physical` : microcode du vertex shader tel que D3D l'a patche.
void ExecuteDraw(const uint8_t* base, uint32_t vs_obj, uint32_t ps_obj, const uint8_t* state, const uint8_t* vs_physical, uint32_t vs_physical_size,
                 uint32_t prim, uint32_t count, const uint8_t* vertices, uint32_t stride) {
  ++g.draws;
  auto skip = [&](const char* why) {
    ++g.skipped;
    if (g.skipped < 20 || g.skipped % 1000 == 0) NGPU_ERR("HUD : dessin %llu ignore : %s (vs %08X ps %08X, %llu ignores)", (unsigned long long)g.draws, why, vs_obj, ps_obj, (unsigned long long)g.skipped);
  };
  if (g.skipping || std::getenv("DASH_HUD_NODRAW") || !EnsureCore()) return;
  if (!vs_obj || !ps_obj) return skip("aucun shader lie");
  const HudObject* vs = RegisterObject(base, vs_obj, false);
  const HudObject* ps = RegisterObject(base, ps_obj, true);
  if (!vs || !ps) return skip("objet shader illisible");
  if (!g.containers.count(vs->key) || !g.containers.count(ps->key)) return skip("shader non traduit");
  BeginFrameIfNeeded();
  if (g.need_clear) {
    // Ni D3D (aucun Clear dans la boucle d'image du HUD) ni le Resolve (drapeaux 0) ne vident la cible : la scene XUI est redessinee entierement a
    // chaque image sur une surface transparente. Sans cet effacement, les images successives s'empilent (traces de l'animation de la boite).
    g.need_clear = false;
    ng::ClearCall clear;
    clear.state.data = state;
    clear.flags = 0xF;
    g.core->renderer().Clear(clear);
  }
  {
    // Premier dessin de chaque image du HUD : un quad plein ecran (sommets deja en pixels : x -0,5..851,5, y -0,5..479,5) qui echantillonne une
    // texture et la multiplie par ColorFactor (gris 0,22, alpha opaque) : la copie atténuée de l'image du titre sous la boite (fond atténué).
    // La texture de ce dessin n'est pas l'image du Dashboard (copie GPU du titre, que le noyau natif garde hors de la memoire invitee) : le
    // dessin n'est pas rendu, son facteur est retenu et l'atténuation est appliquee a l'image du Dashboard a la presentation (HudComposite).
    static uint64_t frame = ~0ull;
    const bool first = frame != g.frames;
    frame = g.frames;
    if (first) g.frame_dim = 1.0f;
    if (first && prim == 13 && count == 4 && stride == 16 && BeF32(vertices) < 0.0f && BeF32(vertices + 16) > 800.0f && BeF32(vertices + 36) > 470.0f) {
      const float factor = BeF32(state + xenos::device::kPixelConstants);
      if (factor > 0.0f && factor < 1.0f) g.frame_dim = factor;
      if (!std::getenv("DASH_HUD_NODIM_SKIP")) return;
    }
  }
  {  // DASH_HUD_DRAWRANGE="a:b" : ne dessine que les dessins a..b de chaque image (bissection)
    static uint64_t frame = ~0ull, k = 0;
    if (frame != g.frames) { frame = g.frames; k = 0; }
    ++k;
    static const char* range = std::getenv("DASH_HUD_DRAWRANGE");
    if (range) {
      unsigned a = 0, b = ~0u;
      std::sscanf(range, "%u:%u", &a, &b);
      if (k < a || k > b) return;
    }
  }
  ng::DrawCall call;
  call.state.data = state;
  call.vs_key = vs->key;
  call.ps_key = ps->key;
  call.vs_physical = vs_physical;
  call.vs_physical_size = vs_physical_size;
  if (prim == 5 || prim == 8 || prim == 13) {
    uint32_t out_count = 0;
    ExpandPrimitive(prim, count, vertices, stride, Elements(*vs, vs_physical, vs_physical_size), g.scratch, out_count);
    if (!out_count) return;
    call.primitive = 4;
    call.count = out_count;
    call.inline_vertices = g.scratch.data();
    call.inline_size = uint32_t(g.scratch.size());
  } else {
    call.primitive = prim;
    call.count = count;
    call.inline_vertices = vertices;
    call.inline_size = count * stride;
  }
  if (Evlog()) {
    const xenos::DeviceState st{state};
    {
      const xenos::TextureFetch t = xenos::DecodeTextureFetch(st, 0);
      if (const char* td = std::getenv("DASH_HUD_TEXDUMP"); td && t.valid && t.format == 2 && !t.tiled) {  // diagnostic : textures 8 bits lineaires en PGM (<td>_<adresse>.pgm)
        static std::map<uint32_t, int> dumped;
        static uint64_t last_frame = ~0ull;
        const bool first_of_frame = last_frame != g.frames;  // (le premier dessin de l'image)
        last_frame = g.frames;
        if ((dumped[t.address]++ == 0 || first_of_frame) && dumped.size() < 40) {
          char name[600];
          std::snprintf(name, sizeof name, "%s_%08X.pgm", td, t.address);
          if (FILE* f = std::fopen(name, "wb")) {
            std::fprintf(f, "P5\n%u %u\n255\n", t.width, t.height);
            const uint8_t* mem = rex::system::kernel_memory()->TranslatePhysical(0) + t.address;
            for (uint32_t y = 0; y < t.height; ++y) std::fwrite(mem + y * t.pitch, 1, t.width, f);
            std::fclose(f);
          }
        }
      }
      std::fprintf(stderr, "[hud]      tex0: valid=%d fmt=%u %ux%u pitch=%u tiled=%d addr=%08X endian=%u swz=%03X filt=%u/%u\n", t.valid, t.format, t.width, t.height, t.pitch, t.tiled, t.address,
                   t.endian, t.swizzle, t.mag_filter, t.min_filter);
    }
    for (uint32_t slot = 1; slot < 16; ++slot) {
      const xenos::TextureFetch t = xenos::DecodeTextureFetch(st, slot);
      if (t.valid) std::fprintf(stderr, "[hud]      tex%u: fmt=%u %ux%u pitch=%u tiled=%d addr=%08X endian=%u\n", slot, t.format, t.width, t.height, t.pitch, t.tiled, t.address, t.endian);
    }
    std::fprintf(stderr, "[hud]      v0:");
    for (int c = 0; c < 8 && uint32_t(c) * 4 + 4 <= count * stride; ++c) std::fprintf(stderr, " %g", BeF32(vertices + 4 * c));
    std::fprintf(stderr, " | vsc0:");
    for (int c = 0; c < 8; ++c) std::fprintf(stderr, " %g", BeF32(state + xenos::device::kVertexConstants + 4 * c));
    std::fprintf(stderr, " | psc0:");
    for (int c = 0; c < 8; ++c) std::fprintf(stderr, " %g", BeF32(state + xenos::device::kPixelConstants + 4 * c));
    std::fputc('\n', stderr);
    std::fprintf(stderr, "[hud] draw %llu prim=%u n=%u stride=%u vs=%016llX ps=%016llX blend=%08X cc=%08X dc=%08X surf=%08X vp=%g,%g,%g,%g tex0=%08X %08X\n",
                 (unsigned long long)g.draws, prim, count, stride, (unsigned long long)vs->key, (unsigned long long)ps->key, st.Reg(0x2201), st.Reg(0x2202),
                 st.Reg(0x2200), st.Reg(0x2000), st.RegF(0x210F), st.RegF(0x2110), st.RegF(0x2111), st.RegF(0x2112), st.U32(xenos::device::kFetchConstants),
                 st.U32(xenos::device::kFetchConstants + 4));
  }
  g.core->renderer().Draw(call);
}

// Cloture le dessin ouvert par BeginVertices (appelee avec g.mutex tenu).
void CloseInline(const uint8_t* base) {
  if (!g.inl.open) return;
  g.inl.open = false;
  if (!g.inl.vertices || !g.inl.count || !g.inl.stride) return;
  ExecuteDraw(base, g.inl.vs, g.inl.ps, g.inl.state.data(), g.inl.vs_physical.data(), uint32_t(g.inl.vs_physical.size()), g.inl.primitive, g.inl.count,
              base + g.inl.vertices, g.inl.stride);
}

// Copie du microcode du vertex shader tel que D3D l'a patche.
bool CopyVsPhysical(const uint8_t* base, uint32_t vs_obj, std::vector<uint8_t>& out) {
  if (!vs_obj) return false;
  const HudObject* vs = RegisterObject(base, vs_obj, false);
  if (!vs) return false;
  out.assign(base + vs->ucode, base + vs->ucode + vs->ucode_size);
  return true;
}

// ---- fin d'image : le noyau soumet, l'image resolue est relue et publiee ----
void PublishOverlay() {
  std::vector<uint8_t> rgba;
  uint32_t w = 0, h = 0;
  if (!g.dest || !g.core->renderer().ReadResolved(g.dest, rgba, w, h)) return;
  bool visible = false;
  {
    size_t pixels = 0;
    uint32_t amax = 0, x0 = w, y0 = h, x1 = 0, y1 = 0;
    for (size_t i = 0; i < size_t(w) * h; ++i) {
      const uint32_t a = rgba[4 * i + 3];
      if (!a) continue;
      ++pixels;
      amax = std::max(amax, a);
      const uint32_t x = uint32_t(i % w), y = uint32_t(i / w);
      x0 = std::min(x0, x); x1 = std::max(x1, x); y0 = std::min(y0, y); y1 = std::max(y1, y);
    }
    // Visible : au moins quelques pixels notablement opaques (une image au repos peut garder des pixels d'alpha tres faible).
    visible = pixels >= 16 && amax >= 8;
    if (std::getenv("DASH_HUD_STATS") && (g.frames % 60 == 1 || Evlog()))
      std::fprintf(stderr, "[hud] overlay image %llu : %zu pixels non transparents, alpha max %u, boite %u,%u - %u,%u\n", (unsigned long long)g.frames, pixels, amax, x0, y0, x1, y1);
  }
  if (const char* dm = std::getenv("DASH_HUD_DUMP")) {  // "<image>:<fichier.ppm>" : overlay sur fond gris (sans transparence) + alpha dans <fichier>.a.pgm
    char* colon = nullptr;
    const unsigned long want = std::strtoul(dm, &colon, 10);
    if (colon && *colon == ':' && want == g.frames) {
      if (FILE* f = std::fopen(colon + 1, "wb")) {
        std::fprintf(f, "P6\n%u %u\n255\n", w, h);
        for (size_t i = 0; i < size_t(w) * h; ++i) {
          const float a = rgba[4 * i + 3] / 255.0f;
          for (int c = 0; c < 3; ++c) {
            const uint8_t v = uint8_t(std::min(255.0f, rgba[4 * i + c] + 96.0f * (1.0f - a)));  // premultiplie sur gris 96
            std::fputc(v, f);
          }
        }
        std::fclose(f);
      }
      if (FILE* f = std::fopen((std::string(colon + 1) + ".a.pgm").c_str(), "wb")) {
        std::fprintf(f, "P5\n%u %u\n255\n", w, h);
        for (size_t i = 0; i < size_t(w) * h; ++i) std::fputc(rgba[4 * i + 3], f);
        std::fclose(f);
      }
    }
  }
  bool changed;
  {
    std::lock_guard<std::mutex> lock(g_overlay.mutex);
    changed = g_overlay.visible != visible;
    g_overlay.rgba = std::move(rgba);
    g_overlay.w = w;
    g_overlay.h = h;
    g_overlay.visible = visible;
    g_overlay.dim = visible ? g.frame_dim : 1.0f;
    ++g_overlay.seq;
  }
  // Notification systeme « interface systeme affichee » (XN_SYS_UI, 9) : sur la console XAM la diffuse aux titres a l'ouverture et a la
  // fermeture du HUD ; le XAM recompile la garde dans ses propres ecouteurs, que le Dashboard (noyau du SDK) ne lit pas.
  if (changed && !std::getenv("DASH_HUD_NONOTIFY")) {
    std::fprintf(stderr, "[hud] HUD %s (image %llu)\n", visible ? "affiche" : "masque", (unsigned long long)g.frames);
    rex::system::kernel_state()->BroadcastNotification(rex::system::XNotificationID(0x00000009), visible ? 1 : 0);
  }
}

// ---- dispatch des fonctions D3D de XAM ----
void Handle(uint32_t addr, PPCContext& ctx, uint8_t* base, void (*orig)(PPCContext&, uint8_t*)) {
  using namespace hud_profile;
  switch (addr) {
    case kBeginVertices: {
      const uint32_t dev = ctx.r3.u32, prim = ctx.r4.u32, count = ctx.r5.u32, stride = ctx.r6.u32;
      {
        std::lock_guard<std::mutex> lock(g.mutex);
        CloseInline(base);
      }
      orig(ctx, base);
      std::lock_guard<std::mutex> lock(g.mutex);
      if (!ctx.r3.u32 || !EnsureCore()) return;
      g.inl.open = true;
      g.inl.vertices = ctx.r3.u32;
      g.inl.primitive = prim;
      g.inl.count = count;
      g.inl.stride = stride;
      g.inl.vs = Be32(base + dev + kVertexShader);
      g.inl.ps = Be32(base + dev + kPixelShader);
      g.inl.state.resize(xenos::kCanonicalStateSize);
      Snapshot(base, dev, g.inl.state.data());
      if (!CopyVsPhysical(base, g.inl.vs, g.inl.vs_physical)) g.inl.vs_physical.clear();
      return;
    }
    case kDrawVerticesUP: {
      if (Evlog() && ctx.r6.u32) {
        std::fprintf(stderr, "[hud] DrawVerticesUP prim=%u n=%u src=%08X stride=%u :", ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32);
        for (uint32_t i = 0; i < std::min(16u, ctx.r5.u32 * ctx.r7.u32 / 4); ++i) std::fprintf(stderr, " %g", BeF32(base + ctx.r6.u32 + 4 * i));
        std::fputc('\n', stderr);
      }
      orig(ctx, base);
      return;
    }
    case kEndVertices: {
      {
        std::lock_guard<std::mutex> lock(g.mutex);
        CloseInline(base);
      }
      orig(ctx, base);
      return;
    }
    case kResolve: {
      const uint32_t dev = ctx.r3.u32, flags = ctx.r4.u32, rect = ctx.r5.u32, point = ctx.r7.u32;
      const double z = ctx.f1.f64;
      {
        std::lock_guard<std::mutex> lock(g.mutex);
        CloseInline(base);
      }
      orig(ctx, base);
      std::lock_guard<std::mutex> lock(g.mutex);
      if (g.skipping || !EnsureCore()) return;
      BeginFrameIfNeeded();
      Snapshot(base, dev, g.shadow);
      ng::ResolveCall resolve;
      resolve.state.data = g.shadow;
      resolve.flags = flags;
      if (rect) {
        resolve.has_rect = true;
        for (int i = 0; i < 4; ++i) resolve.rect[i] = int32_t(Be32(base + rect + 4 * i));
      }
      if (point) {
        resolve.has_dest_point = true;
        resolve.dest_x = int32_t(Be32(base + point));
        resolve.dest_y = int32_t(Be32(base + point + 4));
      }
      resolve.clear_depth = float(z);
      ++g.resolves;
      const uint32_t dest = xenos::DeviceState{g.shadow}.Reg(0x2319);  // RB_COPY_DEST_BASE
      g.dest = dest;
      if (Evlog())
        std::fprintf(stderr, "[hud] resolve flags=%X rect=%d,%d,%d,%d point=%d,%d dest=%08X pitch=%08X surf=%08X color=%08X\n", flags, resolve.rect[0], resolve.rect[1],
                     resolve.rect[2], resolve.rect[3], resolve.dest_x, resolve.dest_y, dest, xenos::DeviceState{g.shadow}.Reg(0x231A),
                     xenos::DeviceState{g.shadow}.Reg(0x2000), xenos::DeviceState{g.shadow}.Reg(0x2001));
      g.core->renderer().Resolve(resolve);
      g.need_clear = true;
      return;
    }
    case kSubmit: {
      {
        std::lock_guard<std::mutex> lock(g.mutex);
        CloseInline(base);
        if (g.in_frame && g.core) {
          g.core->renderer().EndFrame(0);
          g.in_frame = false;
          ++g.frames;
          PublishOverlay();
          if (g.frames == 1 || g.frames % 600 == 0) {
            for (const auto& [what, n] : g.core->renderer().stats()) std::fprintf(stderr, "[hud] image %llu : %s : %u\n", (unsigned long long)g.frames, what.c_str(), n);
            std::fprintf(stderr, "[hud] image %llu : %llu dessins, %llu ignores, %llu resolves\n", (unsigned long long)g.frames, (unsigned long long)g.draws,
                         (unsigned long long)g.skipped, (unsigned long long)g.resolves);
          }
        }
        {
          bool visible;
          {
            std::lock_guard<std::mutex> ol(g_overlay.mutex);
            visible = g_overlay.visible;
          }
          g.skipping = !visible && !std::getenv("DASH_HUD_NOIDLE") && (++g.idle_phase & 3) != 0;
          if (visible) g.idle_phase = 0;
        }
      }
      // Cadence : 60 Hz (la boucle d'image du HUD tourne sinon sans pause, faute de vsync).
      const auto now = std::chrono::steady_clock::now();
      if (g.next_frame < now) g.next_frame = now;
      g.next_frame += std::chrono::microseconds(16667);
      std::this_thread::sleep_until(g.next_frame);
      orig(ctx, base);
      return;
    }
    default:
      orig(ctx, base);
      return;
  }
}

}  // namespace

// Superpose l'overlay du HUD (alpha premultiplie) a l'image du Dashboard (pixels A2B10G10R10 empaquetes du noyau : R bits 0-9, G 10-19,
// B 20-29) ; renvoie false (image inchangee) si le HUD n'a rien a montrer. Interpolation bilineaire de l'overlay (852x480) vers la taille
// de l'image (1280x720).
namespace ngpu {
bool HudComposite(uint32_t* pixels, uint32_t w, uint32_t h) {
  std::lock_guard<std::mutex> lock(g_overlay.mutex);
  if (!g_overlay.visible || g_overlay.rgba.empty() || !w || !h) return false;
  const uint32_t ow = g_overlay.w, oh = g_overlay.h;
  const uint8_t* src = g_overlay.rgba.data();
  const float dim = g_overlay.dim;
  for (uint32_t y = 0; y < h; ++y) {
    const float fy = std::max(0.0f, (y + 0.5f) * oh / h - 0.5f);
    const uint32_t y0 = std::min(oh - 1, uint32_t(fy)), y1 = std::min(oh - 1, y0 + 1);
    const float ty = fy - float(y0);
    for (uint32_t x = 0; x < w; ++x) {
      const float fx = std::max(0.0f, (x + 0.5f) * ow / w - 0.5f);
      const uint32_t x0 = std::min(ow - 1, uint32_t(fx)), x1 = std::min(ow - 1, x0 + 1);
      const float tx = fx - float(x0);
      float c[4];
      for (int k = 0; k < 4; ++k) {
        const float a = src[4 * (size_t(y0) * ow + x0) + k] * (1 - tx) + src[4 * (size_t(y0) * ow + x1) + k] * tx;
        const float b = src[4 * (size_t(y1) * ow + x0) + k] * (1 - tx) + src[4 * (size_t(y1) * ow + x1) + k] * tx;
        c[k] = a * (1 - ty) + b * ty;
      }
      const float alpha = c[3] / 255.0f;
      uint32_t& p = pixels[size_t(y) * w + x];
      if (dim < 1.0f) {  // fond atténué : l'image du Dashboard est multipliee par le facteur avant la superposition
        const float r0 = (p & 1023) * dim, g0 = ((p >> 10) & 1023) * dim, b0 = ((p >> 20) & 1023) * dim;
        p = (p & 0xC0000000u) | uint32_t(b0 + 0.5f) << 20 | uint32_t(g0 + 0.5f) << 10 | uint32_t(r0 + 0.5f);
      }
      if (alpha <= 0.0f && c[0] <= 0.0f && c[1] <= 0.0f && c[2] <= 0.0f) continue;
      const float r = (p & 1023), gch = ((p >> 10) & 1023), b = ((p >> 20) & 1023);
      const uint32_t R = uint32_t(std::clamp(c[0] * (1023.0f / 255.0f) + r * (1 - alpha), 0.0f, 1023.0f) + 0.5f);
      const uint32_t G = uint32_t(std::clamp(c[1] * (1023.0f / 255.0f) + gch * (1 - alpha), 0.0f, 1023.0f) + 0.5f);
      const uint32_t B = uint32_t(std::clamp(c[2] * (1023.0f / 255.0f) + b * (1 - alpha), 0.0f, 1023.0f) + 0.5f);
      p = (p & 0xC0000000u) | B << 20 | G << 10 | R;
    }
  }
  return true;
}
bool HudActive() {
  std::lock_guard<std::mutex> lock(g_overlay.mutex);
  return g_overlay.visible;
}
}  // namespace ngpu

// Point d'entree des crochets de XAM (src_xam_d3d/xam_d3d_hooks.cpp). hudd3d_probe : diagnostics (src/hud_d3d_trace.cpp du projet).
extern "C" bool hudd3d_probe(uint32_t addr, PPCContext& ctx, uint8_t* base, void (*orig)(PPCContext&, uint8_t*));
extern "C" void hudd3d_call(uint32_t addr, PPCContext& ctx, uint8_t* base, void (*orig)(PPCContext&, uint8_t*)) {
  static const bool enabled = std::getenv("DASH_HUD_DISABLE") == nullptr && std::getenv("DASH_NGPU_DISABLE") == nullptr;
  if (!enabled) {
    orig(ctx, base);
    return;
  }
  if (hudd3d_probe(addr, ctx, base, orig)) return;
  Handle(addr, ctx, base, orig);
}

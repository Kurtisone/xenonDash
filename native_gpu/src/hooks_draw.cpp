// native_gpu : état D3D invité suivi côté hôte + dessin.
//   9212EE10  D3DDevice_CreateVertexShader(pFunction) -> D3DVertexShader*
//   9212ED00  D3DDevice_CreatePixelShader(pFunction)  -> D3DPixelShader*
//   9212E7E8  SetVertexShader(dev, shader)            9212E4E8  SetPixelShader(dev, shader)  (inversion par rapport à l'étude)
//   9212E2D8  SetVertexShaderConstantF(dev, reg, pData, nbFloat4)   9212E3C0  idem pixel
//   92123F20  SetTexture(dev, étage, D3DBaseTexture*)
//   925CCB68  DrawVerticesUP(dev, primType, nbSommets, pDonnées, stride)
// Les shaders sont les SPIR-V traduits hors ligne par XenosRecomp (répertoire DASH_NGPU_SHADERS, par défaut
// work/shaders_out), retrouvés par l'adresse du conteneur de shader dans dash.xex.

#include <sys/uio.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <rex/system/kernel_state.h>

#include "guest_tex.h"
#include "ngpu.h"
#include "vk_backend.h"

namespace ngpu {
uint64_t FrameCounter();
}

namespace {

using namespace ngpu;

struct VElem { uint32_t usage, usage_index, offset_bytes, format; bool num_all, comp_all; };
struct ShaderRes {
  bool vertex = false, ok = false;
  uint32_t container = 0;
  std::vector<uint32_t> spv;
  std::vector<VElem> elems;  // éléments de sommet dans l'ordre du conteneur
};

struct TexEntry { HostTexture* tex = nullptr; uint64_t hash = 0; uint64_t checked_frame = ~0ull; FetchConst fc{}; };

struct State {
  uint32_t vs = 0, ps = 0;
  uint32_t tex[16] = {};
  float vsc[1024] = {}, psc[1024] = {};
  std::unordered_map<uint32_t, uint32_t> obj2container;
  std::unordered_map<uint32_t, std::unique_ptr<ShaderRes>> shaders;  // par adresse de conteneur
  std::unordered_map<uint32_t, TexEntry> textures;                   // par adresse invitée de D3DTexture
  uint32_t decl = 0;
  struct DeclElem { uint32_t usage, uidx, offset, type; };
  std::unordered_map<uint32_t, std::vector<DeclElem>> decls;  // objet D3DVertexDeclaration -> éléments
  uint64_t draws = 0, skipped = 0;
} g;

uint8_t* GBase() { return rex::system::kernel_memory()->TranslateVirtual<uint8_t*>(0); }

std::string ShaderDir() {
  const char* e = std::getenv("DASH_NGPU_SHADERS");
  return e && *e ? e : "work/shaders_out";
}

bool ReadFile(const std::string& p, std::vector<uint32_t>& out) {
  std::ifstream f(p, std::ios::binary | std::ios::ate);
  if (!f) return false;
  size_t n = f.tellg(); f.seekg(0);
  out.resize(n / 4);
  f.read(reinterpret_cast<char*>(out.data()), n & ~3ull);
  return true;
}

ShaderRes* GetShader(uint32_t container, bool vertex) {
  auto it = g.shaders.find(container);
  if (it != g.shaders.end()) return it->second.get();
  auto r = std::make_unique<ShaderRes>();
  r->vertex = vertex; r->container = container;
  char name[64];
  std::snprintf(name, sizeof name, "/ui_%08X_%s.spv", container, vertex ? "VS" : "PS");
  r->ok = ReadFile(ShaderDir() + name, r->spv);
  if (!r->ok) { std::snprintf(name, sizeof name, "/video_%08X_%s.spv", container, vertex ? "VS" : "PS"); r->ok = ReadFile(ShaderDir() + name, r->spv); }
  if (!r->ok) NGPU_ERR("SPIR-V introuvable pour le shader %08X (%s) dans %s", container, vertex ? "VS" : "PS", ShaderDir().c_str());
  if (r->ok && vertex) {
    const uint8_t* b = GBase();
    const uint8_t* c = b + container;
    uint32_t vsz = Be32(c + 4), shoff = Be32(c + 0x18);
    const uint8_t* sh = c + shoff;
    uint32_t physoff = Be32(sh), f18 = Be32(sh + 0x18), cnt = Be32(sh + 0x1C);
    const uint8_t* ucode = c + vsz + physoff;
    for (uint32_t i = 0; i < cnt && i < 16; i++) {
      uint32_t v = Be32(sh + 0x24 + 4 * (f18 + i));
      uint32_t addr = v & 0xFFF, usage = (v >> 12) & 0xF, uidx = (v >> 16) & 0xF;
      const uint8_t* ins = ucode + addr * 12;
      uint32_t w1 = Be32(ins + 4), w2 = Be32(ins + 8);
      VElem e{usage, uidx, (w2 & 0x7FFFFF) * 4, (w1 >> 16) & 0x3F, bool((w1 >> 13) & 1), bool((w1 >> 14) & 1)};
      NGPU_LOG("VS %08X élément %u : usage %u/%u adresse %u w0=%08X w1=%08X w2=%08X -> format %u offset %u", container, i, usage, uidx, addr, Be32(ins), w1, w2, e.format, e.offset_bytes);
      r->elems.push_back(e);
    }
  }
  auto* p = r.get();
  g.shaders[container] = std::move(r);
  return p;
}

// Emplacement (location) GLSL imposé par le patch de XenosRecomp (tableau USAGE_LOCATIONS)
int UsageLocation(uint32_t usage, uint32_t idx) {
  switch (usage) {
    case 0: return idx == 0 ? 0 : (idx == 1 ? 15 : -1);                     // Position
    case 3: return idx == 0 ? 1 : -1;                                       // Normal
    case 6: return idx == 0 ? 2 : -1;                                       // Tangent
    case 7: return idx == 0 ? 3 : -1;                                       // Binormal
    case 5: return idx < 4 ? 4 + idx : (idx < 8 ? 8 + idx : -1);            // TexCoord 0-3 -> 4-7 ; 4-7 -> 12-15
    case 10: return idx == 0 ? 8 : (idx == 1 ? 11 : -1);                    // Color
    case 2: return idx == 0 ? 9 : -1;                                       // BlendIndices
    case 1: return idx == 0 ? 10 : -1;                                      // BlendWeight
    default: return -1;
  }
}
bool UsageIsUint(uint32_t usage) { return usage == 2 || usage == 3 || usage == 6 || usage == 7; }

// Décode un élément de sommet invité (grand-boutiste) en 4 mots hôtes.
void DecodeElem(const uint8_t* p, const VElem& e, bool is_uint, uint32_t out[4]) {
  float f[4] = {0, 0, 0, 1};
  uint32_t u[4] = {0, 0, 0, 0};
  auto bf = [&](int i) { return BeF32(p + 4 * i); };
  switch (e.format) {
    case 36: f[0] = bf(0); break;
    case 37: f[0] = bf(0); f[1] = bf(1); break;
    case 57: f[0] = bf(0); f[1] = bf(1); f[2] = bf(2); break;
    case 38: for (int i = 0; i < 4; i++) f[i] = bf(i); break;
    case 6: {  // 8_8_8_8 après échange 8in32 : x = dernier octet en mémoire
      uint8_t b[4] = {p[3], p[2], p[1], p[0]};
      for (int i = 0; i < 4; i++) { f[i] = e.num_all ? float(b[i]) : b[i] / 255.f; u[i] = b[i]; }
      break;
    }
    case 25: case 26: {  // 16_16(_16_16) entiers signés (échange 16in32 : composantes dans l'ordre mémoire des demi-mots)
      int n = e.format == 25 ? 2 : 4;
      for (int i = 0; i < n; i++) { int16_t v = int16_t(Be16(p + 2 * i)); f[i] = e.num_all ? float(v) : v / 32767.f; u[i] = uint16_t(v); }
      break;
    }
    case 31: case 32: {  // 16_16(_16_16)_FLOAT
      int n = e.format == 31 ? 2 : 4;
      for (int i = 0; i < n; i++) {
        uint16_t h = Be16(p + 2 * i); uint32_t s = (h >> 15) & 1, ex = (h >> 10) & 31, m = h & 1023, bits;
        if (!ex) bits = m ? 0 : s << 31; else if (ex == 31) bits = (s << 31) | 0x7F800000u | (m << 13); else bits = (s << 31) | ((ex + 112) << 23) | (m << 13);
        std::memcpy(&f[i], &bits, 4);
      }
      break;
    }
    default: u[0] = Be32(p); break;
  }
  if (is_uint) { u[0] = (e.format == 6 || e.format == 25 || e.format == 26) ? u[0] : Be32(p); std::memcpy(out, u, 16); }
  else std::memcpy(out, f, 16);
}

HostTexture* GetTexture(uint32_t texptr, SamplerDesc& sd) {
  uint8_t* gb = GBase();
  FetchConst fc;
  for (int i = 0; i < 6; i++) fc.d[i] = Be32(gb + texptr + 28 + 4 * i);
  TexInfo ti = DescribeTexture(fc);
  // échantillonneur d'après la fetch constant : clamp (dword0 10..15), filtres (dword3 19..24)
  auto am = [](uint32_t c) -> uint8_t { return c == 0 ? 0 : (c == 1 ? 1 : (c == 3 ? 3 : 2)); };
  sd.addr_u = am((fc.d[0] >> 10) & 7); sd.addr_v = am((fc.d[0] >> 13) & 7);
  sd.linear = !std::getenv("DASH_NGPU_POINT");  // TODO : lire l'état d'échantillonnage (SetSamplerState : 92125D18/92125EA8) ; linéaire par défaut
  if (!ti.ok) return nullptr;
  TexEntry& te = g.textures[texptr];
  uint64_t frame = ngpu::FrameCounter();
  bool same = te.tex && !std::memcmp(&te.fc, &fc, sizeof fc);
  if (same && te.checked_frame == frame) return te.tex;
  size_t sz = TextureDataSize(ti);
  // Les données peuvent résider dans l'une des trois vues de la mémoire physique (0xA/0xC/0xE...) ; lecture protégée
  // (process_vm_readv rend EFAULT au lieu de planter si la plage n'est pas mappée).
  std::vector<uint8_t> raw(sz + 8);
  bool got = false;
  for (uint32_t viewbase : {0xA0000000u, 0xC0000000u, 0xE0000000u}) {
    uint32_t addr = viewbase | (ti.base & 0x1FFFFFFF);
    struct iovec lo{raw.data(), sz}, re{gb + addr, sz};
    if (process_vm_readv(getpid(), &lo, 1, &re, 1, 0) == (ssize_t)sz) { got = true; break; }
  }
  if (!got) { NGPU_LOG("texture %08X : données illisibles (base %08X)", texptr, ti.base); return nullptr; }
  uint64_t h = 1469598103934665603ull;
  const uint64_t* d64 = reinterpret_cast<const uint64_t*>(raw.data());
  for (size_t i = 0; i < sz / 8; i++) { h ^= d64[i]; h *= 1099511628211ull; }
  if (same && te.hash == h) { te.checked_frame = frame; return te.tex; }
  std::vector<uint8_t> rgba;
  if (!DecodeTexture(raw.data(), fc, ti, rgba)) return nullptr;
  HostTexture* nt = VkCreateTexture(ti.width, ti.height, rgba.data());
  if (!nt) return nullptr;
  if (te.tex) VkDestroyTexture(te.tex);
  te.tex = nt; te.hash = h; te.fc = fc; te.checked_frame = frame;
  if (std::getenv("DASH_NGPU_TEXLOG")) std::fprintf(stderr, "[tex %08X] d= %08X %08X %08X %08X %08X %08X\n", texptr, fc.d[0], fc.d[1], fc.d[2], fc.d[3], fc.d[4], fc.d[5]);
  NGPU_LOG("texture %08X : %ux%u format %u %s", texptr, ti.width, ti.height, ti.format, ti.tiled ? "tuilée" : "linéaire");
  return nt;
}

}  // namespace

// ----------------------------------------------------------------------------------------------------------
// Création/sélection des shaders
// ----------------------------------------------------------------------------------------------------------
NGPU_HOOK(9212EE10) {
  uint32_t container = ctx.r3.u32;
  NGPU_ORIG(9212EE10);
  if (ctx.r3.u32) g.obj2container[ctx.r3.u32] = container;
  GetShader(container, true);
  return true;
}
NGPU_HOOK(9212ED00) {
  uint32_t container = ctx.r3.u32;
  NGPU_ORIG(9212ED00);
  if (ctx.r3.u32) g.obj2container[ctx.r3.u32] = container;
  GetShader(container, false);
  return true;
}
// 9212EB48 = D3DDevice_CreateVertexDeclaration(const D3DVERTEXELEMENT9* pElements) : éléments de 12 octets
// {u16 flux, u16 décalage, u32 type (format GPU dans les 6 bits bas), u8 méthode, u8 usage, u8 indice, u8 -}, fin : flux = 0xFF.
NGPU_HOOK(9212EB48) {
  std::vector<State::DeclElem> el;
  uint32_t p = ctx.r3.u32;
  for (int i = 0; i < 16; i++) {
    const uint8_t* e = base + p + 12 * i;
    if (Be16(e) == 0xFF) break;
    el.push_back({e[9], e[10], Be16(e + 2), Be32(e + 4)});
  }
  NGPU_ORIG(9212EB48);
  if (ctx.r3.u32) g.decls[ctx.r3.u32] = el;
  return true;
}
NGPU_HOOK(9212EA00) {
  g.decl = ctx.r4.u32;
  return false;
}
NGPU_HOOK(9212E4E8) { g.ps = ctx.r4.u32; return false; }  // (corrige l'étude : 9212E4E8 = SetPixelShader)
NGPU_HOOK(9212E7E8) { g.vs = ctx.r4.u32; return false; }  //                      9212E7E8 = SetVertexShader
NGPU_HOOK(92123F20) {
  if (ctx.r4.u32 < 16) g.tex[ctx.r4.u32] = ctx.r5.u32;
  return false;
}
NGPU_HOOK(9212E2D8) {
  uint32_t r = ctx.r4.u32, n = ctx.r6.u32;
  if (r < 256 && n <= 256 - r) for (uint32_t i = 0; i < n * 4; i++) g.vsc[r * 4 + i] = BeF32(base + ctx.r5.u32 + 4 * i);
  return false;
}
NGPU_HOOK(9212E3C0) {
  uint32_t r = ctx.r4.u32, n = ctx.r6.u32;
  if (r < 256 && n <= 256 - r) for (uint32_t i = 0; i < n * 4; i++) g.psc[r * 4 + i] = BeF32(base + ctx.r5.u32 + 4 * i);
  return false;
}

// ----------------------------------------------------------------------------------------------------------
// Dessin
// ----------------------------------------------------------------------------------------------------------
static bool DoDraw(uint8_t* base, uint32_t dev, uint32_t prim, uint32_t count, uint32_t data, uint32_t stride, uint32_t lr);

NGPU_HOOK(925CCB68) {
  return DoDraw(base, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, (uint32_t)ctx.lr);
}

// 925CC6B8 = D3DDevice_BeginVertices(dev, primType, nbSommets, stride) -> pointeur d'écriture des sommets ;
// 925CCB58 = D3DDevice_EndVertices(dev) : le dessin est émis ici. (DrawVerticesUP n'appelle pas EndVertices.)
static struct { uint32_t prim = 0, count = 0, stride = 0, ptr = 0; } g_pending;
NGPU_HOOK(925CC6B8) {
  uint32_t prim = ctx.r4.u32, count = ctx.r5.u32, stride = ctx.r6.u32;
  NGPU_ORIG(925CC6B8);
  g_pending = {prim, count, stride, ctx.r3.u32};
  return true;
}
NGPU_HOOK(925CCB58) {
  if (g_pending.ptr) {
    auto p = g_pending; g_pending = {};
    DoDraw(base, ctx.r3.u32, p.prim, p.count, p.ptr, p.stride, (uint32_t)ctx.lr);
  }
  return false;
}

static bool DoDraw(uint8_t* base, uint32_t dev, uint32_t prim, uint32_t count, uint32_t data, uint32_t stride, uint32_t lr) {
  g.draws++;
  if (std::getenv("DASH_NGPU_NODRAW")) return true;
  if (std::getenv("DASH_NGPU_EVLOG") && g.draws < 120) std::fprintf(stderr, "[ev] draw %llu\n", (unsigned long long)g.draws);
  auto skip = [&](const char* why) { g.skipped++; NGPU_LOG("dessin %llu ignoré : %s", (unsigned long long)g.draws, why); return true; };
  auto vo = g.obj2container.find(g.vs), po = g.obj2container.find(g.ps);
  if (vo == g.obj2container.end() || po == g.obj2container.end()) return skip("shader inconnu");
  ShaderRes* vs = GetShader(vo->second, true);
  ShaderRes* ps = GetShader(po->second, false);
  if (!vs->ok || !ps->ok) return skip("SPIR-V absent");
  if (!count || !stride) return true;

  DrawDesc d;
  d.vs_spv = vs->spv.data(); d.vs_words = vs->spv.size(); d.vs_id = vs->container | (1ull << 40);
  if (std::getenv("DASH_NGPU_DRAWLOG") && g.draws <= 12 && g.decl) {
    std::fprintf(stderr, "[decl %08X]", g.decl);
    for (int i = 0; i < 24; i++) std::fprintf(stderr, " %08X", Be32(base + g.decl + 4 * i));
    std::fputc('\n', stderr);
  }
  d.ps_spv = ps->spv.data(); d.ps_words = ps->spv.size(); d.ps_id = ps->container;
  d.vsc = g.vsc; d.psc = g.psc;

  // sommets : un float4/uint4 par élément, expansion des topologies Xenos (rect list, quad list)
  uint32_t ne = (uint32_t)vs->elems.size();
  struct Att { VElem e; int loc; bool u; };
  std::vector<Att> atts;
  auto dit = g.decls.find(g.decl);
  if (dit == g.decls.end()) return skip("déclaration de sommets inconnue");
  for (auto e : vs->elems) {
    bool found = false;
    for (auto& de : dit->second)
      if (de.usage == e.usage && de.uidx == e.usage_index) { e.offset_bytes = de.offset; e.format = de.type & 0x3F; e.num_all = false; found = true; break; }
    if (!found) continue;
    int loc = UsageLocation(e.usage, e.usage_index); if (loc >= 0) atts.push_back({e, loc, UsageIsUint(e.usage)});
  }
  (void)ne;
  const uint32_t hs = (uint32_t)atts.size() * 16;
  auto fetch = [&](uint32_t vi, uint32_t* out) {
    for (size_t a = 0; a < atts.size(); a++) DecodeElem(base + data + vi * stride + atts[a].e.offset_bytes, atts[a].e, atts[a].u, out + a * 4);
  };
  std::vector<uint32_t> verts;
  auto emit = [&](const uint32_t* v) { verts.insert(verts.end(), v, v + hs / 4); };
  std::vector<uint32_t> a(hs / 4 + 4), b(hs / 4 + 4), c(hs / 4 + 4), dd(hs / 4 + 4);
  uint32_t topo = 3;
  switch (prim) {
    case 1: topo = 0; break;
    case 2: topo = 1; break;
    case 3: topo = 2; break;
    case 4: topo = 3; break;
    case 5: topo = 5; break;
    case 6: topo = 4; break;
    case 8: case 13: topo = 3; break;
    default: return skip("type de primitive non géré");
  }
  if (prim == 8) {
    for (uint32_t i = 0; i + 2 < count; i += 3) {
      fetch(i, a.data()); fetch(i + 1, b.data()); fetch(i + 2, c.data());
      for (size_t k = 0; k < atts.size(); k++)
        for (int j = 0; j < 4; j++) {
          if (atts[k].u) { dd[k * 4 + j] = a[k * 4 + j]; continue; }
          float fa, fb, fc2; std::memcpy(&fa, &a[k * 4 + j], 4); std::memcpy(&fb, &b[k * 4 + j], 4); std::memcpy(&fc2, &c[k * 4 + j], 4);
          float r = fb + fc2 - fa; std::memcpy(&dd[k * 4 + j], &r, 4);
        }
      emit(a.data()); emit(b.data()); emit(c.data()); emit(c.data()); emit(b.data()); emit(dd.data());
    }
  } else if (prim == 13) {
    for (uint32_t i = 0; i + 3 < count; i += 4) {
      fetch(i, a.data()); fetch(i + 1, b.data()); fetch(i + 2, c.data()); fetch(i + 3, dd.data());
      emit(a.data()); emit(b.data()); emit(c.data()); emit(a.data()); emit(c.data()); emit(dd.data());
    }
  } else {
    for (uint32_t i = 0; i < count; i++) { fetch(i, a.data()); emit(a.data()); }
  }
  d.topology = topo;
  d.stride = hs;
  d.vdata = verts.data();
  d.vcount = hs ? (uint32_t)(verts.size() / (hs / 4)) : 0;
  for (size_t k = 0; k < atts.size() && k < 16; k++) d.attrs[d.nattrs++] = {(uint32_t)atts[k].loc, atts[k].u, (uint32_t)k * 16};

  // blend / masque d'écriture d'après les registres cachés du périphérique (RB_BLENDCONTROL0 = 0x2201, RB_COLOR_MASK = 0x2104)
  uint32_t bc = Be32(base + dev + 10552), cm = Be32(base + dev + 10460);
  d.blend.src_c = bc & 31; d.blend.op_c = (bc >> 5) & 7; d.blend.dst_c = (bc >> 8) & 31;
  d.blend.src_a = (bc >> 16) & 31; d.blend.op_a = (bc >> 21) & 7; d.blend.dst_a = (bc >> 24) & 31;
  d.blend.enable = !(d.blend.src_c == 1 && d.blend.dst_c == 0 && d.blend.op_c == 0 && d.blend.src_a == 1 && d.blend.dst_a == 0 && d.blend.op_a == 0);
  d.blend.color_mask = cm & 0xF;
  if (std::getenv("DASH_NGPU_NOBLEND")) d.blend = BlendDesc{};

  for (int i = 0; i < 16; i++)
    if (g.tex[i]) d.tex[i] = GetTexture(g.tex[i], d.samp[i]);

  if (std::getenv("DASH_NGPU_DRAWLOG") && g.draws <= 60)
    std::fprintf(stderr, "[draw %llu] prim=%u n=%u stride=%u vs=%08X ps=%08X blend=%08X mask=%X tex0=%08X\n", (unsigned long long)g.draws, prim, count, stride,
                 vo->second, po->second, bc, cm, g.tex[0]);
  if (std::getenv("DASH_NGPU_DRAWLOG") && g.draws <= 4) {
    for (int r = 0; r < 12; r++) std::fprintf(stderr, "   vsc[%d]= %g %g %g %g\n", r, g.vsc[r * 4], g.vsc[r * 4 + 1], g.vsc[r * 4 + 2], g.vsc[r * 4 + 3]);
    for (int r = 0; r < 4; r++) std::fprintf(stderr, "   psc[%d]= %g %g %g %g\n", r, g.psc[r * 4], g.psc[r * 4 + 1], g.psc[r * 4 + 2], g.psc[r * 4 + 3]);
    for (uint32_t i = 0; i < d.vcount && i < 4; i++) { std::fprintf(stderr, "   v%u:", i); for (uint32_t k = 0; k < hs / 4; k++) { float f; std::memcpy(&f, &verts[i * (hs / 4) + k], 4); std::fprintf(stderr, " %g", f); } std::fputc('\n', stderr); }
  }
  VkDraw(d);
  return true;  // la commande PM4 d'origine n'est plus émise
}

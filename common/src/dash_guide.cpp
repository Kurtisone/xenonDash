// xenondash common - lame Guide : composition dans l'image finale.
//
// Voir dash_guide.h pour le pourquoi. Ici : le dessin. Pas de dependance a ImGui (indisponible en
// rendu natif) ni a Vulkan : on ecrit dans un tampon RGBA8, celui que la presentation vient de
// produire. Le texte est rasterise a la main avec une police bitmap 5x7 integrée : c'est laid a
// grande echelle, mais lisible, sans dependance, et suffisant pour un volet de navigation. Une
// version ulterieure pourra rasteriser la vraie police du Dashboard (freetype est dans le SDK).
#include "dash_guide.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include <rex/input/input_system.h>
#include <rex/logging.h>
#include <rex/runtime.h>

namespace dash::guide {
namespace {

// ---------------------------------------------------------------------------------------------
// Police bitmap 5x7 : chaque caractere est une colonne de 5 bits par ligne (7 lignes). Couvre
// l'ASCII imprimable utile. Pas de dependance : c'est le compromis assume pour le rendu natif.
// ---------------------------------------------------------------------------------------------
struct Glyph {
  char c;
  uint8_t rows[7];  // bit 4 = colonne de gauche
};

const Glyph kGlyphs[] = {
    {' ', {0, 0, 0, 0, 0, 0, 0}},
    {'!', {0x04, 0x04, 0x04, 0x04, 0x04, 0x00, 0x04}},
    {'"', {0x0A, 0x0A, 0x00, 0x00, 0x00, 0x00, 0x00}},
    {'\'', {0x04, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00}},
    {'(', {0x02, 0x04, 0x04, 0x04, 0x04, 0x04, 0x02}},
    {')', {0x08, 0x04, 0x04, 0x04, 0x04, 0x04, 0x08}},
    {'+', {0x00, 0x04, 0x04, 0x1F, 0x04, 0x04, 0x00}},
    {',', {0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x08}},
    {'-', {0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00}},
    {'.', {0x00, 0x00, 0x00, 0x00, 0x00, 0x06, 0x06}},
    {'/', {0x10, 0x08, 0x08, 0x04, 0x02, 0x02, 0x01}},
    {'0', {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E}},
    {'1', {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E}},
    {'2', {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F}},
    {'3', {0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E}},
    {'4', {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02}},
    {'5', {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E}},
    {'6', {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E}},
    {'7', {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08}},
    {'8', {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E}},
    {'9', {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C}},
    {':', {0x00, 0x06, 0x06, 0x00, 0x06, 0x06, 0x00}},
    {'=', {0x00, 0x00, 0x1F, 0x00, 0x1F, 0x00, 0x00}},
    {'?', {0x0E, 0x11, 0x01, 0x02, 0x04, 0x00, 0x04}},
    {'A', {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}},
    {'B', {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E}},
    {'C', {0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E}},
    {'D', {0x1C, 0x12, 0x11, 0x11, 0x11, 0x12, 0x1C}},
    {'E', {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F}},
    {'F', {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10}},
    {'G', {0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F}},
    {'H', {0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}},
    {'I', {0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E}},
    {'J', {0x07, 0x02, 0x02, 0x02, 0x02, 0x12, 0x0C}},
    {'K', {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11}},
    {'L', {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F}},
    {'M', {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11}},
    {'N', {0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11}},
    {'O', {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}},
    {'P', {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10}},
    {'Q', {0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D}},
    {'R', {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11}},
    {'S', {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E}},
    {'T', {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04}},
    {'U', {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}},
    {'V', {0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04}},
    {'W', {0x11, 0x11, 0x11, 0x15, 0x15, 0x1B, 0x11}},
    {'X', {0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11}},
    {'Y', {0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04}},
    {'Z', {0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F}},
    {'a', {0x00, 0x00, 0x0E, 0x01, 0x0F, 0x11, 0x0F}},
    {'b', {0x10, 0x10, 0x1E, 0x11, 0x11, 0x11, 0x1E}},
    {'c', {0x00, 0x00, 0x0E, 0x11, 0x10, 0x11, 0x0E}},
    {'d', {0x01, 0x01, 0x0F, 0x11, 0x11, 0x11, 0x0F}},
    {'e', {0x00, 0x00, 0x0E, 0x11, 0x1F, 0x10, 0x0E}},
    {'f', {0x06, 0x09, 0x08, 0x1C, 0x08, 0x08, 0x08}},
    {'g', {0x00, 0x0F, 0x11, 0x11, 0x0F, 0x01, 0x0E}},
    {'h', {0x10, 0x10, 0x1E, 0x11, 0x11, 0x11, 0x11}},
    {'i', {0x04, 0x00, 0x0C, 0x04, 0x04, 0x04, 0x0E}},
    {'j', {0x02, 0x00, 0x06, 0x02, 0x02, 0x12, 0x0C}},
    {'k', {0x10, 0x10, 0x12, 0x14, 0x18, 0x14, 0x12}},
    {'l', {0x0C, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E}},
    {'m', {0x00, 0x00, 0x1A, 0x15, 0x15, 0x11, 0x11}},
    {'n', {0x00, 0x00, 0x1E, 0x11, 0x11, 0x11, 0x11}},
    {'o', {0x00, 0x00, 0x0E, 0x11, 0x11, 0x11, 0x0E}},
    {'p', {0x00, 0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10}},
    {'q', {0x00, 0x0F, 0x11, 0x11, 0x0F, 0x01, 0x01}},
    {'r', {0x00, 0x00, 0x16, 0x19, 0x10, 0x10, 0x10}},
    {'s', {0x00, 0x00, 0x0F, 0x10, 0x0E, 0x01, 0x1E}},
    {'t', {0x08, 0x08, 0x1C, 0x08, 0x08, 0x09, 0x06}},
    {'u', {0x00, 0x00, 0x11, 0x11, 0x11, 0x13, 0x0D}},
    {'v', {0x00, 0x00, 0x11, 0x11, 0x11, 0x0A, 0x04}},
    {'w', {0x00, 0x00, 0x11, 0x11, 0x15, 0x15, 0x0A}},
    {'x', {0x00, 0x00, 0x11, 0x0A, 0x04, 0x0A, 0x11}},
    {'y', {0x00, 0x11, 0x11, 0x11, 0x0F, 0x01, 0x0E}},
    {'z', {0x00, 0x00, 0x1F, 0x02, 0x04, 0x08, 0x1F}},
    {';', {0x00, 0x06, 0x06, 0x00, 0x06, 0x06, 0x04}},
    {'_', {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1F}},
    {'#', {0x0A, 0x0A, 0x1F, 0x0A, 0x1F, 0x0A, 0x0A}},
    {'*', {0x00, 0x04, 0x15, 0x0E, 0x15, 0x04, 0x00}},
    {'<', {0x02, 0x04, 0x08, 0x10, 0x08, 0x04, 0x02}},
    {'>', {0x08, 0x04, 0x02, 0x01, 0x02, 0x04, 0x08}},
};
constexpr int kGlyphCount = int(sizeof(kGlyphs) / sizeof(kGlyphs[0]));

const Glyph* FindGlyph(char c) {
  for (int i = 0; i < kGlyphCount; ++i)
    if (kGlyphs[i].c == c) return &kGlyphs[i];
  return nullptr;
}

// ---------------------------------------------------------------------------------------------
// Dessin dans un tampon RGBA8 (0xAARRGGBB en little-endian : octet bas = R).
// ---------------------------------------------------------------------------------------------
// Les pixels de la presentation sont en R10G10B10A2 (voir front_profile.cpp : le vidage PPM
// extrait 10 bits par canal). On reste donc dans ce format : R = bits 0-9, G = 10-19,
// B = 20-29, A = 30-31. Melange en flottant puis reconversion, sans jamais depasser 1023.
constexpr int kChanMask = 1023;

inline void PutPixel(uint32_t* px, uint32_t w, uint32_t h, int x, int y, uint32_t rgb888,
                     float alpha) {
  if (x < 0 || y < 0 || uint32_t(x) >= w || uint32_t(y) >= h || alpha <= 0.0f) return;
  const size_t i = size_t(y) * w + uint32_t(x);
  const uint32_t dst = px[i];
  const float a = std::min(1.0f, std::max(0.0f, alpha));
  // Source : 8 bits par canal, ramenes sur 10 bits.
  const float sr = float((rgb888 >> 16) & 0xFF) * (kChanMask / 255.0f);
  const float sg = float((rgb888 >> 8) & 0xFF) * (kChanMask / 255.0f);
  const float sb = float(rgb888 & 0xFF) * (kChanMask / 255.0f);
  const float dr = float(dst & kChanMask);
  const float dg = float((dst >> 10) & kChanMask);
  const float db = float((dst >> 20) & kChanMask);
  const uint32_t r = uint32_t(std::min(1023.0f, sr * a + dr * (1.0f - a) + 0.5f));
  const uint32_t g = uint32_t(std::min(1023.0f, sg * a + dg * (1.0f - a) + 0.5f));
  const uint32_t b = uint32_t(std::min(1023.0f, sb * a + db * (1.0f - a) + 0.5f));
  px[i] = r | (g << 10) | (b << 20) | 0xC0000000u;  // alpha = 3 (opaque), comme l'image source
}

void FillRect(uint32_t* px, uint32_t w, uint32_t h, int x0, int y0, int x1, int y1,
              uint32_t rgba, float alpha) {
  if (x0 > x1) std::swap(x0, x1);
  if (y0 > y1) std::swap(y0, y1);
  for (int y = y0; y < y1; ++y)
    for (int x = x0; x < x1; ++x) PutPixel(px, w, h, x, y, rgba, alpha);
}

// Rectangle adouci : attenue l'alpha sur les 2 pixels de bord (evite l'effet « bloc »).
void FillRectSoft(uint32_t* px, uint32_t w, uint32_t h, int x0, int y0, int x1, int y1,
                  uint32_t rgba, float alpha) {
  for (int y = y0; y < y1; ++y) {
    const int dy = std::min(y - y0, y1 - 1 - y);
    for (int x = x0; x < x1; ++x) {
      const int dx = std::min(x - x0, x1 - 1 - x);
      const int d = std::min(dx, dy);
      float a = alpha;
      if (d < 2) a *= 0.55f + 0.225f * float(d);
      PutPixel(px, w, h, x, y, rgba, a);
    }
  }
}

// Texte : echelle entiere, 6 px d'avance par caractere (5 + 1 d'espace).
int TextWidth(const std::string& s, int scale) { return int(s.size()) * 6 * scale; }

void DrawText(uint32_t* px, uint32_t w, uint32_t h, int x, int y, const std::string& s, int scale,
              uint32_t rgba, float alpha) {
  int cx = x;
  for (char ch : s) {
    const Glyph* g = FindGlyph(ch);
    if (g) {
      for (int row = 0; row < 7; ++row) {
        const uint8_t bits = g->rows[row];
        for (int col = 0; col < 5; ++col) {
          if (!(bits & (1u << (4 - col)))) continue;
          FillRect(px, w, h, cx + col * scale, y + row * scale, cx + (col + 1) * scale,
                   y + (row + 1) * scale, rgba, alpha);
        }
      }
    }
    cx += 6 * scale;
  }
}

// Couleurs du Guide (vert Xbox pour l'accent, gris sombre pour le panneau).
constexpr uint32_t kPanel = 0x1A1A1A;
constexpr uint32_t kPanelEdge = 0x3A3A3A;
constexpr uint32_t kAccent = 0x9BCB1F;   // vert Xbox
constexpr uint32_t kText = 0xF2F2F2;
constexpr uint32_t kTextDim = 0x9A9A9A;
constexpr uint32_t kSelected = 0x2E2E2E;

constexpr double kAnimSeconds = 0.18;
constexpr int kPanelWidth = 380;  // largeur du volet, en pixels (base 1280)

}  // namespace

bool Toggle(State& st) {
  if (st.open) {
    Close(st);
    return true;
  }
  Open(st);
  return true;
}

void Open(State& st) {
  st.open = true;
  st.selected = 0;
}

void Close(State& st) { st.open = false; }

bool MoveSelection(State& st, int delta, const std::vector<Item>& items) {
  if (items.empty()) return false;
  const int n = int(items.size());
  const int prev = st.selected;
  st.selected = ((st.selected + delta) % n + n) % n;
  return st.selected != prev;
}

const Item* SelectedItem(const State& st, const std::vector<Item>& items) {
  if (items.empty() || st.selected < 0 || st.selected >= int(items.size())) return nullptr;
  return &items[size_t(st.selected)];
}

bool IsOpen(const State& st) { return st.open; }

bool IsAnimating(const State& st) {
  return st.open ? st.anim < 0.999f : st.anim > 0.001f;
}

bool Draw(State& st, uint32_t* pixels, uint32_t w, uint32_t h, float dt, const PlayerCard& card,
          const std::vector<Item>& items) {
  if (!pixels || w == 0 || h == 0) return false;

  // Animation : glissement depuis la gauche.
  const float target = st.open ? 1.0f : 0.0f;
  const float speed = dt > 0.0f ? std::min(1.0f, dt / float(kAnimSeconds)) : 1.0f;
  if (st.anim < target) st.anim = std::min(target, st.anim + speed);
  else if (st.anim > target) st.anim = std::max(target, st.anim - speed);
  if (st.anim <= 0.0f) return false;

  const float scale = float(w) / 1280.0f;
  const int panel_w = int(kPanelWidth * scale);
  const int x_off = int(-(1.0f - st.anim) * float(panel_w));  // glissement

  const int pad = int(22 * scale);
  const int text_scale = std::max(1, int(2 * scale));
  const int small_scale = std::max(1, int(1 * scale));

  // Fond : voile sombre sur la droite du volet, pour detacher le panneau de l'image du Dashboard.
  for (int x = x_off + panel_w; x < int(w); ++x) {
    const float t = float(x - (x_off + panel_w)) / std::max(1.0f, float(w));
    const float a = 0.35f * st.anim * (1.0f - std::min(1.0f, t * 2.0f));
    if (a > 0.004f)
      for (uint32_t y = 0; y < h; ++y) PutPixel(pixels, w, h, x, int(y), 0x000000, a);
  }

  // Panneau.
  FillRectSoft(pixels, w, h, x_off, 0, x_off + panel_w, int(h), kPanel, 0.97f * st.anim);
  FillRect(pixels, w, h, x_off + panel_w - int(2 * scale), 0, x_off + panel_w, int(h), kPanelEdge,
           0.9f * st.anim);
  // Filet vert en haut (accent Xbox).
  FillRect(pixels, w, h, x_off, 0, x_off + panel_w, int(4 * scale), kAccent, st.anim);

  int y = pad + int(24 * scale);

  // --- carte du joueur ---
  if (card.has_profile && !card.gamertag.empty()) {
    DrawText(pixels, w, h, x_off + pad, y, card.gamertag, text_scale, kText, st.anim);
    y += 7 * text_scale + int(6 * scale);
    const std::string gs = std::to_string(card.gamerscore) + " G";
    DrawText(pixels, w, h, x_off + pad, y, gs, small_scale, kAccent, st.anim);
    y += 7 * small_scale + int(14 * scale);
  } else {
    DrawText(pixels, w, h, x_off + pad, y, "Aucun profil", text_scale, kTextDim, st.anim);
    y += 7 * text_scale + int(14 * scale);
  }

  FillRect(pixels, w, h, x_off + pad, y, x_off + panel_w - pad, y + 1, kPanelEdge, st.anim);
  y += int(14 * scale);

  // --- entrees ---
  for (int i = 0; i < int(items.size()); ++i) {
    const Item& it = items[size_t(i)];
    const int row_h = 7 * text_scale + int(16 * scale);
    const bool sel = (i == st.selected);
    if (sel)
      FillRectSoft(pixels, w, h, x_off + pad - int(8 * scale), y - int(6 * scale),
                   x_off + panel_w - pad + int(8 * scale), y + row_h - int(6 * scale), kSelected,
                   0.85f * st.anim);
    if (sel)
      FillRect(pixels, w, h, x_off + pad - int(8 * scale), y - int(6 * scale),
               x_off + pad - int(5 * scale), y + row_h - int(6 * scale), kAccent, st.anim);
    DrawText(pixels, w, h, x_off + pad + int(4 * scale), y, it.label, text_scale,
             it.enabled ? kText : kTextDim, st.anim * (it.enabled ? 1.0f : 0.6f));
    y += row_h;
  }

  // --- pied : rappel des touches ---
  const int foot_y = int(h) - pad - 7 * small_scale;
  FillRect(pixels, w, h, x_off + pad, foot_y - int(12 * scale), x_off + panel_w - pad,
           foot_y - int(11 * scale), kPanelEdge, st.anim);
  DrawText(pixels, w, h, x_off + pad, foot_y, "Fleches : choisir   A : valider   B : fermer",
           small_scale, kTextDim, st.anim);

  return true;
}

bool GuidePressed() {
  // Bouton Guide de la manette (0x0400) s'il est transmis (--guide_button), ou la touche
  // configuree par keybind_guide (vide par defaut : voir le README pour l'activer).
  static bool was_down = false;
  bool down = false;
  // GetStateForUI : lecture cote hote, sans perturber l'edge tracking du code invite.
  if (auto* rt = rex::Runtime::instance()) {
    if (auto* is = static_cast<rex::input::InputSystem*>(rt->input_system())) {
      rex::input::X_INPUT_STATE st{};
      if (is->GetStateForUI(0, &st) == 0) {
        down = (uint32_t(uint16_t(st.gamepad.buttons)) & 0x0400u) != 0;
      }
    }
  }
  const bool edge = down && !was_down;
  was_down = down;
  return edge;
}

}  // namespace dash::guide

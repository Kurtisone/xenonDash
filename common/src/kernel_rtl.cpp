// xenondash common - fonctions Rtl* de chaînes que le SDK laisse en stub (r3 inchangé = statut aléatoire).
//
// XAM recompilé construit ses chemins de contenu (« Content\<XUID>\<titre>\<type>\... ») avec RtlAppendStringToString : un statut
// quelconque devenait E_UNEXPECTED (0x8000FFFF) dans sub_8185CD38 et la création d'un profil échouait avant tout accès fichier
// (palier N4, docs/xam_recompile.md). Sémantique de ntoskrnl : ajout à la fin du tampon de destination, sans zéro final,
// STATUS_BUFFER_TOO_SMALL si la place manque.

#include <cstdint>
#include <cstring>

#include <rex/hook.h>
#include <rex/ppc/context.h>

namespace {

constexpr uint32_t kStatusBufferTooSmall = 0xC0000023u;

// ANSI_STRING / UNICODE_STRING invités : { u16 Length; u16 MaximumLength; u32 Buffer } en gros-boutiens, longueurs en octets.
struct GuestString {
  uint8_t* p;
  uint16_t Length() const { return __builtin_bswap16(*reinterpret_cast<uint16_t*>(p)); }
  uint16_t Max() const { return __builtin_bswap16(*reinterpret_cast<uint16_t*>(p + 2)); }
  uint32_t Buffer() const { return __builtin_bswap32(*reinterpret_cast<uint32_t*>(p + 4)); }
  void SetLength(uint16_t n) { *reinterpret_cast<uint16_t*>(p) = __builtin_bswap16(n); }
};

// Ajoute `n` octets pris en `src` à `dst`. Vrai si la place suffit.
uint32_t Append(uint8_t* base, uint32_t dst_ptr, const uint8_t* src, uint32_t n) {
  GuestString d{base + dst_ptr};
  if (!n) return 0;
  if (uint32_t(d.Max()) - d.Length() < n) return kStatusBufferTooSmall;
  std::memcpy(base + d.Buffer() + d.Length(), src, n);
  d.SetLength(uint16_t(d.Length() + n));
  return 0;
}

}  // namespace

// RtlAppendStringToString(ANSI_STRING* destination, const ANSI_STRING* source)
REX_HOOK_RAW(__imp__RtlAppendStringToString) {
  const uint32_t dst = ctx.r3.u32, src = ctx.r4.u32;
  if (!dst || !src) {
    ctx.r3.u64 = 0;
    return;
  }
  GuestString s{base + src};
  ctx.r3.u64 = Append(base, dst, base + s.Buffer(), s.Length());
}

// RtlAppendUnicodeStringToString(UNICODE_STRING* destination, const UNICODE_STRING* source)
REX_HOOK_RAW(__imp__RtlAppendUnicodeStringToString) {
  const uint32_t dst = ctx.r3.u32, src = ctx.r4.u32;
  if (!dst || !src) {
    ctx.r3.u64 = 0;
    return;
  }
  GuestString s{base + src};
  ctx.r3.u64 = Append(base, dst, base + s.Buffer(), s.Length());
}

// RtlAppendUnicodeToString(UNICODE_STRING* destination, const wchar_t* source) : source terminée par un zéro (UTF-16 gros-boutien).
REX_HOOK_RAW(__imp__RtlAppendUnicodeToString) {
  const uint32_t dst = ctx.r3.u32, src = ctx.r4.u32;
  if (!dst || !src) {
    ctx.r3.u64 = 0;
    return;
  }
  const auto* w = reinterpret_cast<const uint16_t*>(base + src);
  uint32_t n = 0;
  while (w[n]) ++n;
  ctx.r3.u64 = Append(base, dst, base + src, n * 2);
}


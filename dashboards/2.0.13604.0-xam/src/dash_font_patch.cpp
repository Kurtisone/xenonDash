// Police du Dashboard : glyphe Gamerscore U+E00A (élément XUI GamerScoreIcon, à côté du gamertag).
//
// Le Dashboard choisit lui-même ses polices dans sub_928407F0 (XAM n'y est pour rien) : selon XGetGameRegion (258) et la
// langue (8), il enregistre par sub_921E4C88 (enregistrement d'un typeface XUI ; descripteur : +0 nom, +4 localisateur)
// une police principale et une secondaire parmi « file://media:/Xenon{JK,SC,C}Latin.xtt » (0x92125554, 0x92125594,
// 0x921255D4). Hors région 258, la principale est XenonJKLatin, qui ne contient pas U+E00A dans les polices de la 13604 :
// seule XenonSCLatin le contient. La principale JKLatin est donc remplacée par SCLatin ; le contenu des polices n'est pas
// modifié. DASH_FONT_PATCH=0 désactive le correctif (diagnostic).
//
// Seulement si XenonSCLatin.xtt est importée (media: = <données>/flash) : un import avec les seules polices d'une archive
// XeBuilds 6770 n'a que XenonCLatin et XenonJKLatin, et une principale introuvable effaçait alors TOUT le texte du
// Dashboard (test de l'AppImage, 2026-11-26). Sans SCLatin, JKLatin reste la principale (texte normal, sans l'icône).

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>

REXCVAR_DECLARE(std::string, game_data_root);

extern "C" void __imp__sub_921E4C88(PPCContext& __restrict, uint8_t*);

extern "C" void sub_921E4C88(PPCContext& __restrict ctx, uint8_t* base) {
  constexpr uint32_t kJkLatin = 0x92125554, kScLatin = 0x92125594;
  static const bool off = [] {
    const char* e = std::getenv("DASH_FONT_PATCH");
    if (e && e[0] == '0') return true;
    std::error_code ec;
    const auto sc = std::filesystem::absolute(REXCVAR_GET(game_data_root), ec) / "flash" / "XenonSCLatin.xtt";
    if (!std::filesystem::is_regular_file(sc, ec)) {
      REXLOG_WARN("[dash] police {} absente : XenonJKLatin reste la police principale (pas d'icône Gamerscore ; importer "
                  "les polices de la console avec --fonts-dir)", sc.string());
      return true;
    }
    return false;
  }();
  const uint32_t desc = ctx.r3.u32;
  if (desc) {
    auto* url = reinterpret_cast<uint32_t*>(base + desc + 4);
    if (!off && __builtin_bswap32(*url) == kJkLatin) *url = __builtin_bswap32(kScLatin);
  }
  __imp__sub_921E4C88(ctx, base);
}

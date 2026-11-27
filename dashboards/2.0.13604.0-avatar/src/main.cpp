// avatar - second exécutable du Dashboard Kinect 13604 : AvatarEditor.xex recompilé, mêmes modules XAM/HUD que dash.
#include <cstdlib>

#include "generated/avatar/avatar_init.h"

#include "app.h"

// Comme dash : XAM recompilé, son HUD et ses profils sont toujours actifs.
[[maybe_unused]] static const bool g_xam_defaults = [] {
  setenv("DASH_XAM_MODULE", "2", 0);
  setenv("DASH_PROFILE_MODE", "xam", 0);
  // Imports propres à l'éditeur, servis par XAM recompilé (src/avatar_xam_hooks.cpp).
  setenv("DASH_XAM_REDIRECT",
         "XamAvatarBeginEnumAssets,XamAvatarEnumAssets,XamAvatarEndEnumAssets,XamAvatarGetAssetBinary,XamAvatarGetAssetIcon,"
         "XamAvatarGetInstalledAssetPackageDescription,XamAvatarSetManifest,XamUserCreateAvatarAssetEnumerator,"
         "XamUserValidateAvatarManifest,XamPngDecode,XamPngEncodeEx,XamWriteGamerTile,XamWriteGamerTileEx,XamWriteTile,XamUserProfileSync",
         0);
  return true;
}();

REX_DEFINE_APP(avatar, AvatarApp::Create)

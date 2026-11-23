// Imports `xam.xex` de Guide_AvatarMiniCreator.xex recompilé : routés vers XAM recompilé (palier G).
// FICHIER PRODUIT par tools/gen_module_xam_imports.py à partir du code généré (75 imports) : ne pas éditer.
//
// Chaque `__imp__<Nom>` est masqué : dans la DLL du module, l'appel se lie à cette définition (pas à celle de
// l'exécutable ni du SDK) et passe par dash::xamredir::CallModuleImport, qui exécute l'export de XAM recompilé de même
// ordinal.

#include <cstdint>

#include <rex/ppc/context.h>

#include "dash_xam_redirect.h"

#define MODULE_XAM_IMPORT(name, ordinal)                                                       \
  extern "C" __attribute__((visibility("hidden"))) void __imp__##name(PPCContext& ctx, uint8_t* base) { \
    static dash::xamredir::ModuleImport imp{#name, ordinal};                                    \
    dash::xamredir::CallModuleImport(imp, ctx, base);                                          \
  }

MODULE_XAM_IMPORT(NetDll_WSACleanup, 2)
MODULE_XAM_IMPORT(NetDll_WSAStartup, 1)
MODULE_XAM_IMPORT(ResetEvent, 1067)
MODULE_XAM_IMPORT(RtlGetLastError, 1006)
MODULE_XAM_IMPORT(RtlSetLastError, 1007)
MODULE_XAM_IMPORT(WideCharToMultiByte, 1051)
MODULE_XAM_IMPORT(XGetOverlappedExtendedError, 1082)
MODULE_XAM_IMPORT(XGetOverlappedResult, 1083)
MODULE_XAM_IMPORT(XMsgCancelIORequest, 504)
MODULE_XAM_IMPORT(XMsgInProcessCall, 500)
MODULE_XAM_IMPORT(XMsgStartIORequest, 503)
MODULE_XAM_IMPORT(XamAlloc, 490)
MODULE_XAM_IMPORT(XamAllocEx, 491)
MODULE_XAM_IMPORT(XamAllocSize, 493)
MODULE_XAM_IMPORT(XamAppUnloadSelf, 581)
MODULE_XAM_IMPORT(XamAppUnloadStack, 582)
MODULE_XAM_IMPORT(XamAvatarSetManifest, 1508)
MODULE_XAM_IMPORT(XamBuildDynamicResourceLocator, 798)
MODULE_XAM_IMPORT(XamBuildResourceLocator, 795)
MODULE_XAM_IMPORT(XamEnableSystemAppInput, 406)
MODULE_XAM_IMPORT(XamFree, 492)
MODULE_XAM_IMPORT(XamGetDashContext, 461)
MODULE_XAM_IMPORT(XamGetSystemVersion, 642)
MODULE_XAM_IMPORT(XamInputGetKeystroke, 403)
MODULE_XAM_IMPORT(XamInputGetKeystrokeHud, 409)
MODULE_XAM_IMPORT(XamIsCurrentTitleDash, 462)
MODULE_XAM_IMPORT(XamLaunchAvatarEditor, 2011)
MODULE_XAM_IMPORT(XamLookupCommonStringByIndex, 527)
MODULE_XAM_IMPORT(XamRegisterSysApp, 587)
MODULE_XAM_IMPORT(XamShowCreateProfileUI, 719)
MODULE_XAM_IMPORT(XamShowMessageBox, 745)
MODULE_XAM_IMPORT(XamUnregisterSysApp, 588)
MODULE_XAM_IMPORT(XamUserGetGamerTag, 524)
MODULE_XAM_IMPORT(XamUserGetSigninState, 528)
MODULE_XAM_IMPORT(XamUserGetXUID, 522)
MODULE_XAM_IMPORT(XamWriteGamerTile, 752)
MODULE_XAM_IMPORT(XamWriteGamerTileEx, 763)
MODULE_XAM_IMPORT(XamWriteTile, 753)
MODULE_XAM_IMPORT(XuiAnimRun, 800)
MODULE_XAM_IMPORT(XuiBubbleMessage, 802)
MODULE_XAM_IMPORT(XuiCreateObject, 805)
MODULE_XAM_IMPORT(XuiDestroyObject, 806)
MODULE_XAM_IMPORT(XuiElementAddChild, 808)
MODULE_XAM_IMPORT(XuiElementGetChildById, 810)
MODULE_XAM_IMPORT(XuiElementLayoutTree, 2090)
MODULE_XAM_IMPORT(XuiElementSetBounds, 822)
MODULE_XAM_IMPORT(XuiFindClass, 826)
MODULE_XAM_IMPORT(XuiFreeStringTable, 827)
MODULE_XAM_IMPORT(XuiGetBaseObject, 828)
MODULE_XAM_IMPORT(XuiGetOuter, 831)
MODULE_XAM_IMPORT(XuiInit, 832)
MODULE_XAM_IMPORT(XuiKillTimer, 881)
MODULE_XAM_IMPORT(XuiLoadStringTableFromFile, 834)
MODULE_XAM_IMPORT(XuiLookupStringTableByIndex, 919)
MODULE_XAM_IMPORT(XuiObjectFromHandle, 838)
MODULE_XAM_IMPORT(XuiProcessInput, 841)
MODULE_XAM_IMPORT(XuiRegisterClass, 842)
MODULE_XAM_IMPORT(XuiRenderBegin, 843)
MODULE_XAM_IMPORT(XuiRenderCreateDC, 844)
MODULE_XAM_IMPORT(XuiRenderDestroyDC, 846)
MODULE_XAM_IMPORT(XuiRenderEnd, 847)
MODULE_XAM_IMPORT(XuiRenderGetBackBufferSize, 848)
MODULE_XAM_IMPORT(XuiRenderPresent, 851)
MODULE_XAM_IMPORT(XuiRenderUninit, 853)
MODULE_XAM_IMPORT(XuiResourceClose, 929)
MODULE_XAM_IMPORT(XuiResourceOpen, 927)
MODULE_XAM_IMPORT(XuiResourceRead, 928)
MODULE_XAM_IMPORT(XuiSceneCreate, 855)
MODULE_XAM_IMPORT(XuiSceneNavigateFirst, 857)
MODULE_XAM_IMPORT(XuiSceneNavigateForward, 858)
MODULE_XAM_IMPORT(XuiSendMessage, 863)
MODULE_XAM_IMPORT(XuiSetTimer, 868)
MODULE_XAM_IMPORT(XuiTextElementSetText, 867)
MODULE_XAM_IMPORT(XuiUninit, 865)
MODULE_XAM_IMPORT(XuiUnregisterClass, 866)

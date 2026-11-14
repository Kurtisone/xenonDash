// Imports `xam.xex` de vk.xex recompilé : routés vers XAM recompilé (palier G).
// FICHIER PRODUIT par tools/gen_module_xam_imports.py à partir du code généré (76 imports) : ne pas éditer.
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

MODULE_XAM_IMPORT(GetSystemTimeAsFileTime, 1088)
MODULE_XAM_IMPORT(RtlGetLastError, 1006)
MODULE_XAM_IMPORT(XMsgCompleteIORequest, 501)
MODULE_XAM_IMPORT(XMsgInProcessCall, 500)
MODULE_XAM_IMPORT(XMsgStartIORequestEx, 508)
MODULE_XAM_IMPORT(XUIElementPropVal_Construct, 889)
MODULE_XAM_IMPORT(XUIElementPropVal_Destruct, 890)
MODULE_XAM_IMPORT(XUIElementPropVal_SetString, 891)
MODULE_XAM_IMPORT(XamAlloc, 490)
MODULE_XAM_IMPORT(XamAllocEx, 491)
MODULE_XAM_IMPORT(XamAppUnloadSelf, 581)
MODULE_XAM_IMPORT(XamBuildDynamicResourceLocator, 798)
MODULE_XAM_IMPORT(XamEnableSystemAppInput, 406)
MODULE_XAM_IMPORT(XamFree, 492)
MODULE_XAM_IMPORT(XamGetLanguage, 978)
MODULE_XAM_IMPORT(XamInputGetKeyLocks, 449)
MODULE_XAM_IMPORT(XamInputGetKeystroke, 403)
MODULE_XAM_IMPORT(XamInputGetKeystrokeHudEx, 413)
MODULE_XAM_IMPORT(XamInputResetLayoutKeyboard, 412)
MODULE_XAM_IMPORT(XamInputSetKeyLocks, 448)
MODULE_XAM_IMPORT(XamInputSetKeyboardTranslationHud, 414)
MODULE_XAM_IMPORT(XamInputToggleKeyLocks, 411)
MODULE_XAM_IMPORT(XamLookupCommonStringByIndex, 527)
MODULE_XAM_IMPORT(XamRegisterSysApp, 587)
MODULE_XAM_IMPORT(XamUnregisterSysApp, 588)
MODULE_XAM_IMPORT(XamUserLogon, 523)
MODULE_XAM_IMPORT(XuiAnimRun, 800)
MODULE_XAM_IMPORT(XuiBubbleMessage, 802)
MODULE_XAM_IMPORT(XuiControlGetNavigation, 918)
MODULE_XAM_IMPORT(XuiCreateObject, 805)
MODULE_XAM_IMPORT(XuiCreateSolidBrush, 925)
MODULE_XAM_IMPORT(XuiDestroyBrush, 908)
MODULE_XAM_IMPORT(XuiDestroyObject, 806)
MODULE_XAM_IMPORT(XuiDynamicCast, 807)
MODULE_XAM_IMPORT(XuiEditSetTextFormatInfo, 924)
MODULE_XAM_IMPORT(XuiEditSetTextLimit, 895)
MODULE_XAM_IMPORT(XuiElementGetChildById, 810)
MODULE_XAM_IMPORT(XuiElementGetParent, 817)
MODULE_XAM_IMPORT(XuiElementGetPosition, 991)
MODULE_XAM_IMPORT(XuiElementGetUserFocus, 818)
MODULE_XAM_IMPORT(XuiElementHasFocus, 921)
MODULE_XAM_IMPORT(XuiElementLayoutTree, 2090)
MODULE_XAM_IMPORT(XuiElementSetBounds, 822)
MODULE_XAM_IMPORT(XuiElementSetPosition, 945)
MODULE_XAM_IMPORT(XuiElementSetUserFocus, 824)
MODULE_XAM_IMPORT(XuiFindClass, 826)
MODULE_XAM_IMPORT(XuiFreeStringTable, 827)
MODULE_XAM_IMPORT(XuiFreeUnusedTextures, 948)
MODULE_XAM_IMPORT(XuiGetBaseObject, 828)
MODULE_XAM_IMPORT(XuiGetOuter, 831)
MODULE_XAM_IMPORT(XuiInit, 832)
MODULE_XAM_IMPORT(XuiKillTimer, 881)
MODULE_XAM_IMPORT(XuiLoadStringTableFromFile, 834)
MODULE_XAM_IMPORT(XuiLookupStringTable, 836)
MODULE_XAM_IMPORT(XuiObjectFromHandle, 838)
MODULE_XAM_IMPORT(XuiObjectGetPropertyId, 840)
MODULE_XAM_IMPORT(XuiObjectSetProperty, 892)
MODULE_XAM_IMPORT(XuiProcessInput, 841)
MODULE_XAM_IMPORT(XuiRegisterClass, 842)
MODULE_XAM_IMPORT(XuiRenderBegin, 843)
MODULE_XAM_IMPORT(XuiRenderCreateDC, 844)
MODULE_XAM_IMPORT(XuiRenderDestroyDC, 846)
MODULE_XAM_IMPORT(XuiRenderEnd, 847)
MODULE_XAM_IMPORT(XuiRenderGetBackBufferSize, 848)
MODULE_XAM_IMPORT(XuiRenderPresent, 851)
MODULE_XAM_IMPORT(XuiRenderUninit, 853)
MODULE_XAM_IMPORT(XuiSceneCreate, 855)
MODULE_XAM_IMPORT(XuiSceneNavigateBack, 856)
MODULE_XAM_IMPORT(XuiSceneNavigateFirst, 857)
MODULE_XAM_IMPORT(XuiSendMessage, 863)
MODULE_XAM_IMPORT(XuiSetMessageFilter, 934)
MODULE_XAM_IMPORT(XuiSetTimer, 868)
MODULE_XAM_IMPORT(XuiUninit, 865)
MODULE_XAM_IMPORT(XuiUnregisterClass, 866)
MODULE_XAM_IMPORT(lstrcpynW, 1120)
MODULE_XAM_IMPORT(lstrlenW, 1124)

// Imports `xam.xex` de ximecore.xex recompilé : routés vers XAM recompilé (palier G).
// FICHIER PRODUIT par tools/gen_module_xam_imports.py à partir du code généré (34 imports) : ne pas éditer.
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

MODULE_XAM_IMPORT(CloseHandle, 1044)
MODULE_XAM_IMPORT(CreateFileA, 1095)
MODULE_XAM_IMPORT(GetLastError, 1046)
MODULE_XAM_IMPORT(ReadFile, 1052)
MODULE_XAM_IMPORT(RtlAllocateHeap, 1020)
MODULE_XAM_IMPORT(RtlFreeHeap, 1023)
MODULE_XAM_IMPORT(RtlReAllocateHeap, 1022)
MODULE_XAM_IMPORT(RtlSizeHeap, 1025)
MODULE_XAM_IMPORT(SetLastError, 1049)
MODULE_XAM_IMPORT(WriteFile, 1054)
MODULE_XAM_IMPORT(XMsgCompleteIORequest, 501)
MODULE_XAM_IMPORT(XamAlloc, 490)
MODULE_XAM_IMPORT(XamAllocEx, 491)
MODULE_XAM_IMPORT(XamAllocSize, 493)
MODULE_XAM_IMPORT(XamBuildResourceLocator, 795)
MODULE_XAM_IMPORT(XamFree, 492)
MODULE_XAM_IMPORT(XamGetSystemVersion, 642)
MODULE_XAM_IMPORT(XamInputSetLayoutKeyboard, 410)
MODULE_XAM_IMPORT(XamProfileClose, 568)
MODULE_XAM_IMPORT(XamProfileOpen, 567)
MODULE_XAM_IMPORT(XamRegisterSysApp, 587)
MODULE_XAM_IMPORT(XamTaskCancel, 434)
MODULE_XAM_IMPORT(XamTaskCloseHandle, 433)
MODULE_XAM_IMPORT(XamTaskSchedule, 431)
MODULE_XAM_IMPORT(XamTaskWaitOnCompletion, 436)
MODULE_XAM_IMPORT(XamUnregisterSysApp, 588)
MODULE_XAM_IMPORT(XamUserGetXUID, 522)
MODULE_XAM_IMPORT(XdfLoadXexFromCache, 1621)
MODULE_XAM_IMPORT(XuiResourceClose, 929)
MODULE_XAM_IMPORT(XuiResourceOpen, 927)
MODULE_XAM_IMPORT(XuiResourceRead, 928)
MODULE_XAM_IMPORT(XuiResourceSeek, 951)
MODULE_XAM_IMPORT(lstrcpyW, 1118)
MODULE_XAM_IMPORT(lstrlenW, 1124)

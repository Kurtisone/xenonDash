// xenondash common - accès à la fonction du SDK qu'un REX_HOOK_RAW de l'exécutable remplace.
//
// Sous Linux, librexruntime.so référence ses propres exports par la GOT : la table de registre du SDK
// (rex::ppc::FindPPCFuncByName) pointe sur le hook de l'exécutable (interposition ELF) et un repli par cette table
// s'appellerait lui-même. dlsym(RTLD_NEXT) depuis l'exécutable renvoie la définition suivante, celle du SDK. Sous
// Windows, les DLL ne s'interposent pas : la table de registre suffit.
#pragma once

#if !defined(_WIN32)
#include <dlfcn.h>
#endif

#include <rex/ppc/context.h>
#include <rex/ppc/function.h>

namespace dash {

inline PPCFunc* SdkFunction(const char* sym) {
#if defined(_WIN32)
  return rex::ppc::FindPPCFuncByName(sym);
#else
  return reinterpret_cast<PPCFunc*>(dlsym(RTLD_NEXT, sym));
#endif
}

}  // namespace dash

// xenondash common - réglages de console (XConfig) : ce qu'un Dashboard déclare au common (kernel_xconfig.cpp).
#pragma once

#include <cstdint>

namespace dash::xconfig {

// Table des fuseaux horaires du Dashboard chargé (adresse invitée, nombre d'entrées), au format de 2.0.13604.0 (entrées de 32
// octets, décrites dans kernel_xconfig.cpp). Le fuseau de l'hôte est alors rendu sous la forme de l'entrée de cette table qui a les
// mêmes règles, noms compris, comme sur une console réglée sur ce fuseau. Sans table, noms vides. À appeler depuis un
// initialiseur statique (avant la première lecture d'un réglage).
void SetConsoleTimeZoneTable(uint32_t address, uint32_t count);

}  // namespace dash::xconfig

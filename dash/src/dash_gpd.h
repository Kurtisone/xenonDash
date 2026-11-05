// dash - lecture des fichiers GPD d'un profil Xbox 360 (conteneur XDBF).
//
// Un profil console (paquet STFS) contient :
//   FFFE07D1.gpd      : données du Dashboard, dont la liste des jeux joués (espace 4 « titres ») ;
//   <TitleID>.gpd     : un fichier par jeu, avec ses succès (espace 1) et ses images (espace 2 :
//                       0x8000 = icône du jeu, sinon images de succès).
//
// Format XDBF (big-endian) :
//   en-tête 0x18 : 'XDBF', version, capacité de la table d'entrées, nb d'entrées,
//                  capacité de la table libre, nb d'entrées libres
//   entrée 0x12  : espace u16, id u64, décalage u32, taille u32
//   données      : après les deux tables (décalages relatifs à ce point)

#pragma once

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace dash::gpd {

enum Namespace : uint16_t { kAchievement = 1, kImage = 2, kSetting = 3, kTitle = 4, kString = 5 };

constexpr uint64_t kTitleImageId = 0x8000;

struct Achievement {
  uint32_t id = 0, image_id = 0, flags = 0;
  int32_t gamerscore = 0;
  uint64_t unlock_time = 0;  // FILETIME
  std::u16string name, unlocked_desc, locked_desc;
  bool earned() const { return (flags & 0x00030000) != 0; }
};

struct TitlePlayed {
  uint32_t title_id = 0, achievements_possible = 0, achievements_earned = 0;
  uint32_t gamerscore_total = 0, gamerscore_earned = 0, flags = 0;
  uint16_t reserved = 0;
  uint8_t avatar[6] = {};
  uint64_t last_played = 0;  // FILETIME
  std::u16string name;
};

class File {
 public:
  bool Load(const std::filesystem::path& path) {
    data_.clear();
    entries_.clear();
    FILE* f = std::fopen(path.string().c_str(), "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    data_.resize(size_t(n > 0 ? n : 0));
    data_.resize(std::fread(data_.data(), 1, data_.size(), f));
    std::fclose(f);
    if (data_.size() < 0x18 || std::string(data_.begin(), data_.begin() + 4) != "XDBF") return false;
    uint32_t cap = Be32(8), count = Be32(12), free_cap = Be32(16);
    size_t table = 0x18, start = 0x18 + size_t(cap) * 0x12 + size_t(free_cap) * 8;
    for (uint32_t i = 0; i < count && table + (i + 1) * 0x12 <= data_.size(); i++) {
      size_t e = table + i * 0x12;
      uint16_t ns = Be16(e);
      uint64_t id = uint64_t(Be32(e + 2)) << 32 | Be32(e + 6);
      size_t off = start + Be32(e + 10), len = Be32(e + 14);
      if (off + len <= data_.size()) entries_[{ns, id}] = {off, len};
    }
    return true;
  }

  std::optional<std::vector<uint8_t>> Get(uint16_t ns, uint64_t id) const {
    auto it = entries_.find({ns, id});
    if (it == entries_.end()) return std::nullopt;
    return std::vector<uint8_t>(data_.begin() + it->second.first,
                                data_.begin() + it->second.first + it->second.second);
  }

  std::vector<Achievement> Achievements() const {
    std::vector<Achievement> out;
    for (auto& [key, loc] : entries_) {
      if (key.first != kAchievement || loc.second < 0x1C) continue;
      size_t o = loc.first;
      Achievement a;
      a.id = Be32(o + 4);
      a.image_id = Be32(o + 8);
      a.gamerscore = int32_t(Be32(o + 12));
      a.flags = Be32(o + 16);
      a.unlock_time = uint64_t(Be32(o + 20)) << 32 | Be32(o + 24);
      size_t p = o + Be32(o), end = o + loc.second;  // la taille de la structure précède les chaînes
      a.name = ReadW(p, end);
      a.unlocked_desc = ReadW(p, end);
      a.locked_desc = ReadW(p, end);
      out.push_back(std::move(a));
    }
    return out;
  }

  std::vector<TitlePlayed> Titles() const {
    std::vector<TitlePlayed> out;
    for (auto& [key, loc] : entries_) {
      if (key.first != kTitle || loc.second < 0x28) continue;
      size_t o = loc.first;
      TitlePlayed t;
      t.title_id = Be32(o);
      t.achievements_possible = Be32(o + 4);
      t.achievements_earned = Be32(o + 8);
      t.gamerscore_total = Be32(o + 12);
      t.gamerscore_earned = Be32(o + 16);
      t.reserved = Be16(o + 20);
      for (int i = 0; i < 6; i++) t.avatar[i] = data_[o + 22 + i];
      t.flags = Be32(o + 28);
      t.last_played = uint64_t(Be32(o + 32)) << 32 | Be32(o + 36);
      size_t p = o + 0x28, end = o + loc.second;
      t.name = ReadW(p, end);
      if (t.title_id) out.push_back(std::move(t));
    }
    return out;
  }

 private:
  uint32_t Be32(size_t o) const {
    return uint32_t(data_[o]) << 24 | uint32_t(data_[o + 1]) << 16 | uint32_t(data_[o + 2]) << 8 |
           data_[o + 3];
  }
  uint16_t Be16(size_t o) const { return uint16_t(data_[o] << 8 | data_[o + 1]); }
  std::u16string ReadW(size_t& p, size_t end) const {
    std::u16string s;
    while (p + 1 < end) {
      char16_t c = char16_t(Be16(p));
      p += 2;
      if (!c) break;
      s += c;
    }
    return s;
  }

  std::vector<uint8_t> data_;
  std::map<std::pair<uint16_t, uint64_t>, std::pair<size_t, size_t>> entries_;
};

}  // namespace dash::gpd

// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "platform/storage.h"

#include <cstdint>
#include <string>
#include <unordered_map>

namespace playden::steam {

// What the Library needs about a Steam app it only knows from a license: whether it is a game or a demo
// and its name. Anything else (DLC, tools, configs) is kept as kOther, so it is never looked up again.
struct AppName {
  enum class Kind : std::uint8_t { kOther = 0, kGame = 1, kDemo = 2 };
  Kind kind = Kind::kOther;
  std::string name;  // empty for kOther
  std::string icon;  // the PICS icon hash, when known
  std::uint64_t learned_at = 0;  // unix seconds
};

using AppNames = std::unordered_map<std::uint32_t, AppName>;

inline constexpr std::size_t kMaxAppNames = 20'000;

// "app-names.cache" in user storage. Names are public data, shared by every account on the console.
// Load returns nothing for a missing, damaged or foreign file; Save keeps the newest kMaxAppNames.
AppNames LoadAppNames(const Storage& storage);
bool SaveAppNames(const Storage& storage, const AppNames& names);

}  // namespace playden::steam

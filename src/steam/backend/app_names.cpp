// SPDX-License-Identifier: GPL-3.0-or-later
#include "steam/backend/app_names.h"

#include <algorithm>
#include <vector>

namespace playden::steam {
namespace {

constexpr const char* kPath = "app-names.cache";
const Bytes kMagic = {'P', 'D', 'N', 'A', 'M', 'E', 0, 1};
constexpr std::size_t kMaxName = 1024;
constexpr std::size_t kMaxIcon = 64;
constexpr std::size_t kMaxBytes = 4 * 1024 * 1024;

std::uint32_t Checksum(const Bytes& bytes, std::size_t size) {
  std::uint32_t crc = 0xffffffffu;
  for (std::size_t i = 0; i < size; ++i) {
    crc ^= bytes[i];
    for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
  }
  return ~crc;
}

void Number(Bytes& out, std::uint64_t value, unsigned count) {
  for (unsigned i = 0; i < count; ++i) {
    out.push_back(static_cast<std::uint8_t>(value));
    value >>= 8;
  }
}

struct Reader {
  const Bytes& bytes;
  std::size_t offset = 0;
  std::size_t limit = 0;
  bool okay = true;
  std::uint64_t Number(unsigned count) {
    if (!okay || offset > limit || count > limit - offset) {
      okay = false;
      return 0;
    }
    std::uint64_t value = 0;
    for (unsigned i = 0; i < count; ++i) value |= std::uint64_t(bytes[offset++]) << (i * 8);
    return value;
  }
  std::string String(std::size_t maximum) {
    const auto count = Number(2);
    if (!okay || count > maximum || offset > limit || count > limit - offset) {
      okay = false;
      return {};
    }
    std::string value(bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                      bytes.begin() + static_cast<std::ptrdiff_t>(offset + count));
    offset += count;
    return value;
  }
};

}  // namespace

AppNames LoadAppNames(const Storage& storage) {
  const auto read = storage.Read(kPath, kMaxBytes);
  if (!read.ok()) return {};
  const Bytes& bytes = read.value();
  if (bytes.size() < kMagic.size() + 8 || !std::equal(kMagic.begin(), kMagic.end(), bytes.begin())) return {};
  const std::size_t body = bytes.size() - 4;
  Reader crc{bytes, body, bytes.size()};
  if (static_cast<std::uint32_t>(crc.Number(4)) != Checksum(bytes, body)) return {};
  Reader reader{bytes, kMagic.size(), body};
  const auto count = reader.Number(4);
  if (!reader.okay || count > kMaxAppNames) return {};
  AppNames names;
  names.reserve(static_cast<std::size_t>(count));
  for (std::uint64_t i = 0; i < count; ++i) {
    const auto id = static_cast<std::uint32_t>(reader.Number(4));
    AppName name;
    const auto kind = reader.Number(1);
    name.learned_at = reader.Number(8);
    name.name = reader.String(kMaxName);
    name.icon = reader.String(kMaxIcon);
    if (!reader.okay || !id || kind > 2) return {};
    name.kind = static_cast<AppName::Kind>(kind);
    if (name.kind != AppName::Kind::kOther && name.name.empty()) return {};
    names[id] = std::move(name);
  }
  if (reader.offset != body) return {};
  return names;
}

bool SaveAppNames(const Storage& storage, const AppNames& names) {
  std::vector<std::pair<std::uint32_t, const AppName*>> entries;
  entries.reserve(names.size());
  for (const auto& [id, name] : names) {
    if (!id || name.name.size() > kMaxName || name.icon.size() > kMaxIcon) continue;
    if (name.kind != AppName::Kind::kOther && name.name.empty()) continue;
    entries.emplace_back(id, &name);
  }
  // The newest stay, as many as Load accepts: kMaxAppNames entries in kMaxBytes.
  std::sort(entries.begin(), entries.end(),
            [](const auto& a, const auto& b) { return a.second->learned_at > b.second->learned_at; });
  std::size_t bytes = kMagic.size() + 4 + 4;
  std::size_t kept = 0;
  for (; kept < entries.size() && kept < kMaxAppNames; ++kept) {
    const auto& name = *entries[kept].second;
    const std::size_t size = 4 + 1 + 8 + 2 + name.name.size() + 2 + name.icon.size();
    if (bytes + size > kMaxBytes) break;
    bytes += size;
  }
  entries.resize(kept);
  std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
  Bytes out = kMagic;
  Number(out, entries.size(), 4);
  for (const auto& [id, name] : entries) {
    Number(out, id, 4);
    Number(out, static_cast<std::uint8_t>(name->kind), 1);
    Number(out, name->learned_at, 8);
    Number(out, name->name.size(), 2);
    out.insert(out.end(), name->name.begin(), name->name.end());
    Number(out, name->icon.size(), 2);
    out.insert(out.end(), name->icon.begin(), name->icon.end());
  }
  Number(out, Checksum(out, out.size()), 4);
  return storage.WriteAtomic(kPath, out).ok();
}

}  // namespace playden::steam

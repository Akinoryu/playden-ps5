// SPDX-License-Identifier: GPL-3.0-or-later
#include "library/update_check.h"

#include "library/store.h"

#include <limits>
#include <string>
#include <string_view>

namespace playden {
namespace {

constexpr std::size_t kMaximumBytes = 1024 * 1024;
constexpr std::size_t kMaximumGames = 20000;

std::string Path(std::uint64_t account) { return "updates-" + std::to_string(account) + ".v1"; }

// Plain decimal, no sign or leading zero (but "0"); the console's libc has no stream parsing.
std::optional<std::uint64_t> Decimal(std::string_view text) {
  if (text.empty() || text.size() > 20 || (text.size() > 1 && text[0] == '0')) return std::nullopt;
  std::uint64_t value = 0;
  for (const char c : text) {
    if (c < '0' || c > '9') return std::nullopt;
    const std::uint64_t digit = static_cast<std::uint64_t>(c - '0');
    if (value > (std::numeric_limits<std::uint64_t>::max() - digit) / 10) return std::nullopt;
    value = value * 10 + digit;
  }
  return value;
}

}  // namespace

bool operator==(const SavedGameUpdate& left, const SavedGameUpdate& right) {
  return left.newer == right.newer && left.new_dlc == right.new_dlc && left.download_bytes == right.download_bytes;
}
bool operator==(const SavedUpdateCheck& left, const SavedUpdateCheck& right) {
  return left.checked_at == right.checked_at && left.games == right.games;
}

Result<void> UpdateCheckStore::Save(std::uint64_t account, const SavedUpdateCheck& check) const {
  if (!account) return Result<void>::Failure(StorageError::kInvalidPath, "Invalid update check account.");
  if (check.checked_at < 0 || check.games.size() > kMaximumGames)
    return Result<void>::Failure(StorageError::kInvalidData, "Update check cannot be saved.");
  std::string text = "version=1\nchecked_at=" + std::to_string(check.checked_at) + "\n";
  for (const auto& [id, game] : check.games) {
    if (!id.valid() || (!game.newer && !game.new_dlc)) continue;  // only games with something to download
    text += ToKey(id) + " " + std::to_string((game.newer ? 1 : 0) | (game.new_dlc ? 2 : 0)) + " " +
            std::to_string(game.download_bytes) + "\n";
  }
  const auto ready = storage_.EnsureRootDirectory();
  if (!ready.ok()) return ready;
  return storage_.WriteAtomicPrivate(Path(account), Bytes(text.begin(), text.end()));
}

Result<std::optional<SavedUpdateCheck>> UpdateCheckStore::Load(std::uint64_t account) const {
  using Loaded = Result<std::optional<SavedUpdateCheck>>;
  if (!account) return Loaded::Failure(StorageError::kInvalidPath, "Invalid update check account.");
  const auto read = storage_.Read(Path(account), kMaximumBytes);
  if (!read.ok()) {
    if (read.error() == StorageError::kNotFound) return Loaded::Success(std::nullopt);
    return Loaded::Failure(read.error(), read.message());
  }
  const auto invalid = [] { return Loaded::Failure(StorageError::kInvalidData, "Update check file is damaged."); };
  const std::string text(read.value().begin(), read.value().end());
  if (text.empty() || text.back() != '\n') return invalid();
  SavedUpdateCheck check;
  std::size_t start = 0;
  std::size_t line_number = 0;
  while (start < text.size()) {
    const auto end = text.find('\n', start);
    const std::string_view line(text.data() + start, end - start);
    start = end + 1;
    if (line_number++ == 0) {
      if (line != "version=1") return invalid();
      continue;
    }
    if (line_number == 2) {
      constexpr std::string_view kPrefix = "checked_at=";
      if (line.substr(0, kPrefix.size()) != kPrefix) return invalid();
      const auto when = Decimal(line.substr(kPrefix.size()));
      if (!when || *when > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) return invalid();
      check.checked_at = static_cast<std::int64_t>(*when);
      continue;
    }
    const auto first = line.find(' ');
    const auto second = first == std::string_view::npos ? first : line.find(' ', first + 1);
    if (second == std::string_view::npos) return invalid();
    const auto id = ParseKey(line.substr(0, first));
    const auto flags = Decimal(line.substr(first + 1, second - first - 1));
    const auto bytes = Decimal(line.substr(second + 1));
    if (!id || !flags || *flags < 1 || *flags > 3 || !bytes || check.games.size() >= kMaximumGames) return invalid();
    if (!check.games.emplace(*id, SavedGameUpdate{(*flags & 1) != 0, (*flags & 2) != 0, *bytes}).second) return invalid();
  }
  if (line_number < 2) return invalid();
  return Loaded::Success(std::move(check));
}

}  // namespace playden

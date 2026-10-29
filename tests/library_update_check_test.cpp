// SPDX-License-Identifier: GPL-3.0-or-later
#include "library/update_check.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

#include "steam_id.h"

using namespace playden;

namespace {

void Check(bool value, const char* message) {
  if (!value) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    std::exit(1);
  }
}

void Write(Storage& storage, const std::string& path, const std::string& text) {
  Check(storage.WriteAtomic(path, Bytes(text.begin(), text.end())).ok(), "write fixture");
}

}  // namespace

int main() {
  char root[] = "/tmp/playden-update-check-XXXXXX";
  Check(mkdtemp(root), "temporary root");
  Storage storage(root);
  UpdateCheckStore store(storage);

  // Nothing saved yet: an empty check, not an error.
  auto missing = store.Load(76561198000000001ull);
  Check(missing.ok() && !missing.value(), "an account without a file has no check");

  // A round trip keeps the time and each game's kind and download size.
  SavedUpdateCheck saved;
  saved.checked_at = 1'800'000'000;
  saved.games[SteamId(1091500)] = {true, false, 2'100'000'000};
  saved.games[SteamId(548430)] = {false, true, 120'000'000};
  saved.games[SteamId(220)] = {true, true, 0};
  saved.games[SteamId(440)] = {false, false, 50};  // nothing to download: not saved
  Check(store.Save(76561198000000001ull, saved).ok(), "save a check");
  auto loaded = store.Load(76561198000000001ull);
  Check(loaded.ok() && loaded.value(), "load the check back");
  auto expected = saved;
  expected.games.erase(SteamId(440));
  Check(*loaded.value() == expected, "time, kinds and sizes survive the round trip");
  struct stat info{};
  Check(stat((std::string(root) + "/updates-76561198000000001.v1").c_str(), &info) == 0 && (info.st_mode & 0777) == 0600,
        "the file is owner-only");

  // Accounts don't see each other's checks.
  Check(store.Load(76561198000000002ull).ok() && !store.Load(76561198000000002ull).value(),
        "another account has none");
  SavedUpdateCheck other;
  other.checked_at = 1'700'000'000;
  other.games[SteamId(620)] = {true, false, 7};
  Check(store.Save(76561198000000002ull, other).ok(), "save the other account's check");
  Check(*store.Load(76561198000000001ull).value() == expected && *store.Load(76561198000000002ull).value() == other,
        "each account reads its own");

  // A never-finished check with no games still loads.
  Check(store.Save(76561198000000003ull, {}).ok(), "save an empty check");
  Check(store.Load(76561198000000003ull).value()->checked_at == 0, "an empty check loads");

  // Damaged files fail the load.
  const char* damaged[] = {
      "", "version=2\nchecked_at=1\n", "version=1\n", "version=1\nchecked_at=-1\n", "version=1\nchecked_at=01\n",
      "version=1\nchecked_at=1", "version=1\nchecked_at=1\n440 0 5\n", "version=1\nchecked_at=1\n440 4 5\n",
      "version=1\nchecked_at=1\n440 1\n", "version=1\nchecked_at=1\nabc 1 5\n", "version=1\nchecked_at=1\n440 1 5\n440 2 6\n",
      "version=1\nchecked_at=1\n440 1 99999999999999999999999\n"};
  for (const char* text : damaged) {
    Write(storage, "updates-76561198000000004.v1", text);
    const auto result = store.Load(76561198000000004ull);
    Check(!result.ok() && result.error() == StorageError::kInvalidData, "a damaged file is refused");
  }
  Write(storage, "updates-76561198000000004.v1", "version=1\nchecked_at=5\n440 1 5\nlocal-0123abcd-0000-4000-8000-000000000000 2 9\n");
  const auto mixed = store.Load(76561198000000004ull);
  Check(mixed.ok() && mixed.value()->games.size() == 2, "game keys of any store parse");

  Check(!store.Save(0, saved).ok() && !store.Load(0).ok(), "account 0 is refused");
  std::filesystem::remove_all(root);
  std::puts("library_update_check_test passed");
  return 0;
}

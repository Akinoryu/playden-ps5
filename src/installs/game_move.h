// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Moving an installed game to another volume: the install service's MoveRunner (InstallService::Move).
//
// The game's folder <from> is copied to <to> (<games_root>/<key> on the destination) through the copy
// journal <to>/.playden-copy, as "Copy into Playden" copies an imported game (stores/local/copy_plan.h):
// the plan of every file is saved first, each file is copied to .playden-copy/<index>.part, synced, read
// back and compared with what was read from <from> (size and SHA-1), then renamed into place. A resume
// skips files already at their planned size; when the game changed some since, their copies are deleted
// before the new plan is saved. Once every file is there, the switch marker
// (kMoveSwitchName) is written and the game's PS5 title follows the copy (GameMoveOptions::relocate,
// RuntimeHost::Relocate on the console): with the title stopped and under its publication lease, files
// the game changed since they were copied are copied again, the install record's file list and the
// record itself (last) are copied, and the title's record names <to>. The service then lists the game
// at <to>, and Finish deletes <from> and the journal.
//
// Folders and files in <to> are made through platform::ManagedDirectory, with the policy of the
// destination's games folder: kUnownedTree on a drive that records no owner or permissions (exFAT),
// kApprovedTree otherwise (GamesFolderPolicy). A copy keeps each file's and folder's mode where the
// destination records it. The title's own publication picks its game-folder rule again when it
// follows the copy (settings_publication: unowned on such a drive), in either direction.
#include "installs/install_service.h"
#include "platform/managed_directory.h"
#include "steam/install_volumes.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace playden::installs {

// What GameMoveOptions::relocate is asked: the game's PS5 title, if it has one, plays from `to`.
struct GameRelocation {
  GameId game;
  std::string to;         // the copy, as this process opens it
  std::string volume_id;  // the install volume of `to`
  // Run once the title is stopped (at once for a game without a title): copies again what changed
  // since the copy and finishes it. Empty, or why not. Safe to run again after it succeeded.
  std::function<std::string()> move_files;
};
struct GameRelocationResult {
  enum class Outcome { kDone, kBusy, kFailed };
  Outcome outcome = Outcome::kDone;
  std::string message;
};

// Where a move can stop as a crash would (GameMoveOptions::checkpoint).
enum class MoveCheckpoint {
  kCopied,        // every file copied and checked; no switch marker yet
  kSwitchMarked,  // the switch marker written; the title not asked yet
  kSwitched,      // the title follows the copy; the service doesn't know yet
  kOldDeleted,    // Finish deleted the old folder; the copy's journal is still there
};

struct GameMoveOptions {
  std::function<void(const std::string&)> log;
  // Free space of the drive holding a folder; empty: steam::QueryVolumeSpace.
  std::function<std::optional<steam::VolumeSpace>(const std::string& path)> space;
  std::size_t buffer_bytes = 1024 * 1024;
  // Has the game's title follow the copy (RuntimeHost::Relocate). Empty: no game has a title
  // (move_files runs at once).
  std::function<GameRelocationResult(const GameRelocation&)> relocate;
  // Held around each ManagedDirectory call in the copy, never while its bytes are written: a root raise
  // on another thread (ps5::WithRoot) would fail its ownership checks or leave a root-owned part
  // (ps5::HoldEffectiveUid on the console). Empty holds nothing.
  std::function<std::shared_ptr<void>()> hold_effective_uid;
  // Tests: false stops the move there, as a crash would (Copy returns canceled, Finish a failure).
  std::function<bool(MoveCheckpoint)> checkpoint;
  // Tests: after each file reaches its final name in the copy.
  std::function<void(const std::string& path)> after_file;
};

// The ManagedDirectory policy for a games folder: kUnownedTree when files made there don't keep
// their owner and mode (platform::RecordsOwnership: exFAT), kApprovedTree otherwise or when unknown.
platform::DirectoryPolicy GamesFolderPolicy(const std::string& games_root);

class GameMover final : public MoveRunner {
 public:
  explicit GameMover(GameMoveOptions options = {});

  InstallRunner::Result Copy(const GameId& id, const std::string& from, const std::string& to, const std::string& volume_id,
                             const std::atomic<bool>& cancel,
                             const std::function<void(std::uint64_t done, std::uint64_t total, std::uint64_t held)>& progress,
                             const std::function<void(InstallStep)>& step,
                             const std::function<void(std::uint64_t done, std::uint64_t total)>& checked) override;
  std::string Finish(const std::string& from, const std::string& to) override;
  std::string Discard(const std::string& from, const std::string& to) override;

 private:
  void Log(const std::string& line) const;
  bool Reached(MoveCheckpoint checkpoint) const;
  GameMoveOptions options_;
};

}  // namespace playden::installs

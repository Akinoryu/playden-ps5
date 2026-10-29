#pragma once
// A store's library for tests: every Refresh answers at once with its games, or with a failure.
#include "stores/library_source.h"

#include <deque>
#include <mutex>
#include <vector>

namespace playden::test {

class FakeLibrarySource final : public stores::LibrarySource {
 public:
  explicit FakeLibrarySource(Store store = Store::kLocal) : store_(store) {}
  std::vector<GameSummary> games;
  bool fail = false;
  int refreshes = 0;

  Store store() const override { return store_; }
  std::uint64_t Refresh(std::uint64_t account, std::uint64_t generation) override {
    std::lock_guard<std::mutex> lock(mutex_);
    ++refreshes;
    LibraryEvent event;
    event.kind = fail ? LibraryEventKind::kFailure : LibraryEventKind::kComplete;
    event.request_id = ++request_;
    event.account_id = account;
    event.account_generation = generation;
    event.error = fail ? steam::LibraryError::kPersistence : steam::LibraryError::kNone;
    if (!fail) event.games = games;
    event.licenses_read = !fail;
    events_.push_back(std::move(event));
    return request_;
  }
  std::uint64_t LoadCached(std::uint64_t account, std::uint64_t generation) override {
    return Refresh(account, generation);
  }
  std::uint64_t Clear(std::uint64_t account, std::uint64_t generation) override {
    std::lock_guard<std::mutex> lock(mutex_);
    LibraryEvent event;
    event.kind = LibraryEventKind::kCleared;
    event.request_id = ++request_;
    event.account_id = account;
    event.account_generation = generation;
    events_.push_back(std::move(event));
    return request_;
  }
  void Invalidate(std::uint64_t) override {}
  void Shutdown() override {}
  std::vector<LibraryEvent> Pump(std::size_t max_events) override {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<LibraryEvent> result;
    while (!events_.empty() && result.size() < max_events) {
      result.push_back(std::move(events_.front()));
      events_.pop_front();
    }
    return result;
  }

 private:
  Store store_;
  std::mutex mutex_;
  std::deque<LibraryEvent> events_;
  std::uint64_t request_ = 0;
};

inline GameSummary ImportedGame(const char* uuid, const char* title) {
  GameSummary game;
  game.id = GameId::Local(uuid);
  game.title = title;
  return game;
}

}  // namespace playden::test

#pragma once

#include "library/model.h"
#include "steam/session_store.h"

#include <atomic>
#include <vector>

namespace playden::steam {

enum class LibraryError {
  kNone,
  kCanceled,
  kNetwork,
  kProtocol,
  kCredentialsRevoked,
  kNoSession,
  kPersistence,
};

struct LibraryResult {
  LibraryError error = LibraryError::kProtocol;
  std::vector<playden::GameSummary> games;
  bool metadata_partial = false;
  // The account's licenses were read with the games (Steam's CM answered): the library says what it owns.
  bool licenses_read = false;
};

class LibraryBackend {
 public:
  virtual ~LibraryBackend() = default;
  virtual LibraryResult Fetch(const StoredSession& session,
                              const std::atomic<bool>& cancel) = 0;
};

}  // namespace playden::steam

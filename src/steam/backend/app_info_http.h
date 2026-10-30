// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "steam/backend/http_client.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace playden::steam {

// Steam's PICS answers most apps with only the SHA-1 and size of their app info VDF; the Steam client then
// fetches the VDF from the clientconfig host by that hash. The copy is checked against both, so the host
// is not trusted.
inline constexpr const char* kAppInfoHttpHost = "clientconfig.akamai.steamstatic.com";

struct HashedAppInfo {
  std::vector<std::uint8_t> sha;  // SHA-1 of the VDF text without trailing NULs
  std::uint32_t size = 0;         // gzip's uncompressed size, trailing NUL included
};

using AppInfoPerform = std::function<HttpResponse(const HttpRequest&, const std::atomic<bool>&)>;

struct AppInfoHttpStats {
  std::size_t fetched = 0;
  std::size_t failed = 0;    // answered, but not a matching copy (status, gzip, size or SHA-1)
  std::size_t skipped = 0;   // a transport failure or timeout on that app, or not asked after the stop
  std::string error;         // the last transport failure, empty when none
  std::uint32_t new_connections = 0;
  std::uint64_t total_ms = 0;  // summed over requests
};

// "https://<host>/appinfo/<id>/sha/<hex>.txt.gz"; a host that isn't a bare name gets the default one.
std::string AppInfoHttpUrl(std::string host, std::uint32_t app_id, const std::vector<std::uint8_t>& sha);

// Fetches the VDFs of `apps` by hash, up to `workers` at once. Each worker gets its own transport from
// `make_perform` (one HttpClient per thread). An app whose request fails is skipped; after three transport
// failures in a row the rest are skipped too, since the host is then out of reach. `on_text`, when set, is
// called (from a worker, one call at a time) for each checked copy as it lands.
std::map<std::uint32_t, std::string> FetchAppInfoByHash(
    const std::function<AppInfoPerform()>& make_perform, const std::string& host,
    const std::map<std::uint32_t, HashedAppInfo>& apps, std::size_t workers, const std::atomic<bool>& cancel,
    AppInfoHttpStats* stats = nullptr,
    const std::function<void(std::uint32_t, const std::string&)>& on_text = {});

}  // namespace playden::steam

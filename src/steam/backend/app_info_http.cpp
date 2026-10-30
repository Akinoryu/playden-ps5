// SPDX-License-Identifier: GPL-3.0-or-later
#include "steam/backend/app_info_http.h"

#include "steam/backend/gzip.h"

#include <mbedtls/sha1.h>

#include <algorithm>
#include <mutex>
#include <thread>

namespace playden::steam {
namespace {

constexpr std::uint32_t kTimeoutMs = 5'000;  // a few KB each
constexpr int kFailuresBeforeStop = 3;

// A bare host name for a URL: letters, digits, dots and dashes only.
bool ValidHost(const std::string& host) {
  if (host.empty() || host.size() > 253) return false;
  return std::all_of(host.begin(), host.end(), [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-';
  });
}

std::string Hex(const std::vector<std::uint8_t>& bytes) {
  static constexpr char kDigits[] = "0123456789abcdef";
  std::string text;
  text.reserve(bytes.size() * 2);
  for (const auto byte : bytes) {
    text.push_back(kDigits[byte >> 4]);
    text.push_back(kDigits[byte & 15]);
  }
  return text;
}

}  // namespace

std::string AppInfoHttpUrl(std::string host, std::uint32_t app_id, const std::vector<std::uint8_t>& sha) {
  if (!ValidHost(host)) host = kAppInfoHttpHost;
  return "https://" + host + "/appinfo/" + std::to_string(app_id) + "/sha/" + Hex(sha) + ".txt.gz";
}

std::map<std::uint32_t, std::string> FetchAppInfoByHash(
    const std::function<AppInfoPerform()>& make_perform, const std::string& host,
    const std::map<std::uint32_t, HashedAppInfo>& apps, std::size_t workers, const std::atomic<bool>& cancel,
    AppInfoHttpStats* stats, const std::function<void(std::uint32_t, const std::string&)>& on_text) {
  std::map<std::uint32_t, std::string> texts;
  AppInfoHttpStats totals;
  if (apps.empty() || !make_perform) {
    totals.skipped = apps.size();
    if (stats) *stats = totals;
    return texts;
  }
  std::vector<std::pair<std::uint32_t, const HashedAppInfo*>> queue;
  queue.reserve(apps.size());
  for (const auto& [id, app] : apps) queue.emplace_back(id, &app);

  std::mutex mutex;  // guards texts, totals, next, failures_in_row and on_text calls
  std::size_t next = 0;
  int failures_in_row = 0;
  const auto work = [&] {
    const AppInfoPerform perform = make_perform();
    for (;;) {
      std::uint32_t id = 0;
      const HashedAppInfo* app = nullptr;
      {
        std::lock_guard<std::mutex> lock(mutex);
        if (next == queue.size() || cancel.load() || failures_in_row >= kFailuresBeforeStop) return;
        id = queue[next].first;
        app = queue[next].second;
        ++next;
      }
      if (app->sha.size() != 20 || app->size == 0 || app->size > kMaxGunzipBytes || !perform) {
        std::lock_guard<std::mutex> lock(mutex);
        ++totals.failed;
        continue;
      }
      HttpRequest request;
      request.url = AppInfoHttpUrl(host, id, app->sha);
      request.headers = {{"User-Agent", "Playden/1.0"}};
      request.max_response_bytes = kMaxGunzipBytes;
      request.timeout_ms = kTimeoutMs;
      const HttpResponse response = perform(request, cancel);
      std::string text;
      bool matched = false;
      if (response.ok() && response.status_code == 200) {
        std::vector<std::uint8_t> vdf;
        if (Gunzip(response.body.data(), response.body.size(), app->size, &vdf)) {
          text.assign(vdf.begin(), vdf.end());
          while (!text.empty() && text.back() == '\0') text.pop_back();
          unsigned char sha1[20] = {};
          mbedtls_sha1(reinterpret_cast<const unsigned char*>(text.data()), text.size(), sha1);
          matched = std::equal(sha1, sha1 + sizeof(sha1), app->sha.begin());
        }
      }
      std::lock_guard<std::mutex> lock(mutex);
      totals.new_connections += response.timing.new_connections;
      totals.total_ms += response.timing.total_ms;
      if (!response.ok()) {
        if (response.error != HttpError::kCanceled) {
          totals.error = HttpFailureName(response);
          ++failures_in_row;
        }
        ++totals.skipped;
        continue;
      }
      failures_in_row = 0;
      if (!matched) {
        ++totals.failed;
        continue;
      }
      ++totals.fetched;
      if (on_text) on_text(id, text);
      texts[id] = std::move(text);
    }
  };
  const std::size_t count = std::max<std::size_t>(1, std::min(workers, queue.size()));
  std::vector<std::thread> threads;
  threads.reserve(count - 1);
  for (std::size_t index = 1; index < count; ++index) threads.emplace_back(work);
  work();
  for (auto& thread : threads) thread.join();
  totals.skipped += queue.size() - next;
  if (stats) *stats = totals;
  return texts;
}

}  // namespace playden::steam

// SPDX-License-Identifier: GPL-3.0-or-later
//
// Chromium's check of the system's memory, included by shim.c after sampler.c.
//
// Every 5 seconds Chromium's memory pressure check (SystemMemoryPressureEvaluator) asks Windows how
// much memory is free (GlobalMemoryStatusEx) on its browser thread, which in BeamNG is the game's
// main thread, inside cef_do_message_loop_work. On the console (build 27, two hours of play) a call
// into CEF of about 25 ms came every 5.01 s, and with it 332 of the 334 frames of 34 to 40 ms. So
// the shim answers libcef_real.dll's calls with Windows' last answer while it is under a minute
// old. It times each call it passes on, logs the first with what Windows said and any that took
// over 5 ms, and every 10 s how many calls there were. shim:no-memory-cache passes every call on.

typedef BOOL(WINAPI* memory_status_fn)(void* status);
static memory_status_fn g_real_memory_status;
static volatile long g_memory_lock;
static unsigned char g_memory_answer[64];  // MEMORYSTATUSEX
static int64_t g_memory_answer_at;         // when Windows gave it
static volatile long g_memory_calls, g_memory_passed, g_memory_logged;

static void MemoryLock(void) {
  while (__sync_lock_test_and_set(&g_memory_lock, 1)) Sleep(0);
}
static void MemoryUnlock(void) { __sync_lock_release(&g_memory_lock); }

static void MemoryCopy(unsigned char* to, const unsigned char* from) {
  for (int i = 0; i < 64; ++i) to[i] = from[i];
}

// MEMORYSTATUSEX: dwLength, dwMemoryLoad, then the total and free bytes of physical memory, the
// page file and the virtual address space.
static void LogMemoryAnswer(const unsigned char* status, int64_t took, int64_t frequency) {
  static const char* const kinds[] = {"; physical ", "; page file ", "; virtual "};
  const uint64_t* bytes = (const uint64_t*)(status + 8);
  char line[300];
  size_t n = SampleAppend(line, 0, sizeof(line), "libcef-shim: memory: GlobalMemoryStatusEx for CEF took ");
  n = SampleNumber(line, n, sizeof(line), (uint64_t)(took * 1000 / frequency));
  n = SampleAppend(line, n, sizeof(line), " ms: load ");
  n = SampleNumber(line, n, sizeof(line), *(const DWORD*)(status + 4));
  n = SampleAppend(line, n, sizeof(line), "%");
  for (int i = 0; i < 3; ++i) {
    n = SampleAppend(line, n, sizeof(line), kinds[i]);
    n = SampleNumber(line, n, sizeof(line), bytes[2 * i + 1] >> 20);
    n = SampleAppend(line, n, sizeof(line), " of ");
    n = SampleNumber(line, n, sizeof(line), bytes[2 * i] >> 20);
    n = SampleAppend(line, n, sizeof(line), " MiB free");
  }
  SampleAppend(line, n, sizeof(line), "\n");
  Log(line);
}

static BOOL WINAPI MemoryStatusHook(void* status) {
  static int64_t frequency;
  if (!frequency) QueryPerformanceFrequency(&frequency);
  if (!status || *(DWORD*)status != 64 || !frequency) return g_real_memory_status(status);
  int64_t now = PaceNow();
  __sync_add_and_fetch(&g_memory_calls, 1);
  if (!g_no_memory_cache) {
    MemoryLock();
    BOOL fresh = g_memory_answer_at && now - g_memory_answer_at < 60 * frequency;
    if (fresh) MemoryCopy(status, g_memory_answer);
    MemoryUnlock();
    if (fresh) return 1;
  }
  BOOL answered = g_real_memory_status(status);
  int64_t took = PaceNow() - now;
  __sync_add_and_fetch(&g_memory_passed, 1);
  if (!answered) return answered;
  MemoryLock();
  MemoryCopy(g_memory_answer, status);
  g_memory_answer_at = PaceNow();
  MemoryUnlock();
  long logged = g_memory_logged;
  if (!logged || (took * 200 > frequency && logged < 12)) {
    __sync_add_and_fetch(&g_memory_logged, 1);
    LogMemoryAnswer(status, took, frequency);
  }
  return answered;
}

// From cef_initialize, once libcef_real.dll is loaded.
static void HookMemoryCheck(HMODULE real) {
  g_real_memory_status = (memory_status_fn)PatchImport(real, "GlobalMemoryStatusEx", (void*)MemoryStatusHook);
  Log(!g_real_memory_status ? "libcef-shim: memory: CEF imports no GlobalMemoryStatusEx\n"
      : g_no_memory_cache   ? "libcef-shim: memory: CEF's GlobalMemoryStatusEx calls are timed\n"
                            : "libcef-shim: memory: CEF's GlobalMemoryStatusEx calls get answers under a minute old\n");
}

// Every 10 s, from the watchdog's thread.
static void LogMemoryChecks(void) {
  long calls = __sync_lock_test_and_set(&g_memory_calls, 0), passed = __sync_lock_test_and_set(&g_memory_passed, 0);
  if (!calls) return;
  char line[160];
  size_t n = SampleAppend(line, 0, sizeof(line), "libcef-shim: memory: CEF asked how much memory is free ");
  n = SampleNumber(line, n, sizeof(line), (uint64_t)calls);
  n = SampleAppend(line, n, sizeof(line), " times in 10 s, ");
  n = SampleNumber(line, n, sizeof(line), (uint64_t)passed);
  SampleAppend(line, n, sizeof(line), " passed on to Windows\n");
  Log(line);
}

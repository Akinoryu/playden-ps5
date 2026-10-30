// SPDX-License-Identifier: GPL-3.0-or-later
//
// The game's sleeps and clock reads, included by shim.c.
//
// In each stall build 23 caught on the console, BeamNG's main thread was in Sleep, or reading
// QueryPerformanceCounter, in one function of BeamNG.drive.x64.exe that waits for a time to come,
// while every other thread of the game was idle: the game was waiting for time to pass, not for
// work. Either it asks to wait about 250 ms, or a short Sleep comes back that late. To tell:
// - BeamNG.drive.x64.exe's Sleep import is wrapped. Every 10 s a line counts the main thread's
//   calls, how long they asked for and took, and the longest, with where it came from; the other
//   threads' calls are counted. A main-thread Sleep that took over 50 ms more than it asked for is
//   logged as it happens;
// - a thread of the shim's own sleeps 2 ms at a time and logs each sleep that took over 50 ms, and
//   when GetTickCount64 and QueryPerformanceCounter disagree by as much;
// - the main thread's short sleeps (1 to 4 ms) wait by yielding the CPU until the time has come,
//   which needs no timer, so a late timer cannot hold the game; shim:real-sleep turns that off;
// - its QueryPerformanceCounter import is counted, as a call takes about 1 us on the console;
// - the code of the function the main thread waited in, and of the four that called it, is logged
//   in hex, with the game's imports that deal with time, to read what it waits for.

typedef void(WINAPI* sleep_fn)(DWORD);
typedef BOOL(WINAPI* qpc_fn)(int64_t*);
static sleep_fn g_real_sleep;
static qpc_fn g_real_qpc;
static int64_t g_qpc_frequency;

static struct {
  // The main thread's, written only by it.
  long main_calls, spun;
  int64_t main_asked, main_took, longest_took;
  DWORD longest_asked;
  uint64_t longest_from;
  long late_logged;
  // Any thread's, counted without a lock: close enough.
  volatile long other_calls, qpc_calls;
} g_sleeps;

static BOOL RealQpc(int64_t* counter) { return (g_real_qpc ? g_real_qpc : QueryPerformanceCounter)(counter); }

static BOOL WINAPI GameQpc(int64_t* counter) {
  ++g_sleeps.qpc_calls;
  return g_real_qpc(counter);
}

static void WINAPI GameSleep(DWORD ms) {
  if (!g_main_thread || GetCurrentThreadId() != g_main_thread) {
    ++g_sleeps.other_calls;
    g_real_sleep(ms);
    return;
  }
  int64_t start = 0, end = 0;
  RealQpc(&start);
  if (ms && ms <= 4 && !g_real_sleep_only) {
    int64_t until = start + (int64_t)ms * g_qpc_frequency / 1000;
    do {
      SwitchToThread();
      RealQpc(&end);
    } while (end < until);
    ++g_sleeps.spun;
  } else {
    g_real_sleep(ms);
    RealQpc(&end);
  }
  int64_t took = end - start, asked = (int64_t)ms * g_qpc_frequency / 1000;
  ++g_sleeps.main_calls;
  g_sleeps.main_asked += asked;
  g_sleeps.main_took += took;
  uint64_t from = (uint64_t)(uintptr_t)__builtin_return_address(0);
  if (took > g_sleeps.longest_took) {
    g_sleeps.longest_took = took;
    g_sleeps.longest_asked = ms;
    g_sleeps.longest_from = from;
  }
  if (took - asked > g_qpc_frequency / 20 && g_sleeps.late_logged < 40) {
    ++g_sleeps.late_logged;
    char line[300];
    size_t n = SampleAppend(line, 0, sizeof(line), "libcef-shim: sleep: the main thread asked for ");
    n = SampleNumber(line, n, sizeof(line), ms);
    n = SampleAppend(line, n, sizeof(line), " ms and slept ");
    n = SampleNumber(line, n, sizeof(line), (uint64_t)(took * 1000 / g_qpc_frequency));
    n = SampleAppend(line, n, sizeof(line), " ms at ");
    n = SampleNumber(line, n, sizeof(line), (uint64_t)((start - g_start_qpc) * 10 / g_qpc_frequency / 10));
    n = SampleAppend(line, n, sizeof(line), ".");
    n = SampleNumber(line, n, sizeof(line), (uint64_t)((start - g_start_qpc) * 10 / g_qpc_frequency % 10));
    n = SampleAppend(line, n, sizeof(line), " s, from ");
    n = FormatAddress(line, n, sizeof(line), from);
    SampleAppend(line, n, sizeof(line), "\n");
    Log(line);
  }
}

// Every 10 s: the game's sleeps and clock reads.
static void SleepSummary(long watched, int64_t watch_longest) {
  static char line[600];
  long calls = g_sleeps.main_calls, spun = g_sleeps.spun, others = g_sleeps.other_calls, qpc = g_sleeps.qpc_calls;
  int64_t asked = g_sleeps.main_asked, took = g_sleeps.main_took, longest = g_sleeps.longest_took;
  DWORD longest_asked = g_sleeps.longest_asked;
  uint64_t from = g_sleeps.longest_from;
  g_sleeps.main_calls = g_sleeps.spun = 0;
  g_sleeps.main_asked = g_sleeps.main_took = g_sleeps.longest_took = 0;
  g_sleeps.other_calls = g_sleeps.qpc_calls = 0;
  size_t n = SampleAppend(line, 0, sizeof(line), "libcef-shim: sleep: since the last line the main thread slept ");
  n = SampleNumber(line, n, sizeof(line), (uint64_t)calls);
  n = SampleAppend(line, n, sizeof(line), " times (");
  n = SampleNumber(line, n, sizeof(line), (uint64_t)spun);
  n = SampleAppend(line, n, sizeof(line), " by yielding), asking for ");
  n = SampleNumber(line, n, sizeof(line), (uint64_t)(asked * 1000 / g_qpc_frequency));
  n = SampleAppend(line, n, sizeof(line), " ms and taking ");
  n = SampleNumber(line, n, sizeof(line), (uint64_t)(took * 1000 / g_qpc_frequency));
  n = SampleAppend(line, n, sizeof(line), " ms");
  if (calls) {
    n = SampleAppend(line, n, sizeof(line), ", the longest ");
    n = SampleNumber(line, n, sizeof(line), (uint64_t)(longest * 1000 / g_qpc_frequency));
    n = SampleAppend(line, n, sizeof(line), " ms for ");
    n = SampleNumber(line, n, sizeof(line), longest_asked);
    n = SampleAppend(line, n, sizeof(line), " asked, from ");
    n = FormatAddress(line, n, sizeof(line), from);
  }
  n = SampleAppend(line, n, sizeof(line), "; other threads slept ");
  n = SampleNumber(line, n, sizeof(line), (uint64_t)others);
  n = SampleAppend(line, n, sizeof(line), " times; QueryPerformanceCounter read ");
  n = SampleNumber(line, n, sizeof(line), (uint64_t)qpc);
  n = SampleAppend(line, n, sizeof(line), " times; the shim's own 2 ms sleeps: ");
  n = SampleNumber(line, n, sizeof(line), (uint64_t)watched);
  n = SampleAppend(line, n, sizeof(line), ", the longest ");
  n = SampleNumber(line, n, sizeof(line), (uint64_t)(watch_longest * 1000 / g_qpc_frequency));
  SampleAppend(line, n, sizeof(line), " ms\n");
  Log(line);
}

// The bytes of the function of the game's program that holds rva (from its unwind table), 32 a line.
static void DumpGameFunction(const unsigned char* program, uint32_t rva) {
  uint64_t base = 0;
  const uint32_t* entry = (const uint32_t*)RtlLookupFunctionEntry((uint64_t)(uintptr_t)program + rva, &base, NULL);
  char line[200];
  if (!entry || base != (uint64_t)(uintptr_t)program) {
    size_t n = SampleAppend(line, 0, sizeof(line), "libcef-shim: code: no function holds ");
    n = HitchHex(line, n, sizeof(line), rva);
    SampleAppend(line, n, sizeof(line), "\n");
    Log(line);
    return;
  }
  uint32_t begin = entry[0], end = entry[1];
  if (end - begin > 4096) end = begin + 4096;
  size_t n = SampleAppend(line, 0, sizeof(line), "libcef-shim: code: function ");
  n = HitchHex(line, n, sizeof(line), begin);
  n = SampleAppend(line, n, sizeof(line), " to ");
  n = HitchHex(line, n, sizeof(line), entry[1]);
  n = SampleAppend(line, n, sizeof(line), ", holding ");
  n = HitchHex(line, n, sizeof(line), rva);
  SampleAppend(line, n, sizeof(line), "\n");
  Log(line);
  for (uint32_t at = begin; at < end; at += 32) {
    n = SampleAppend(line, 0, sizeof(line), "libcef-shim: code ");
    n = HitchHex(line, n, sizeof(line), at);
    n = SampleAppend(line, n, sizeof(line), ":");
    for (uint32_t i = at; i < at + 32 && i < end; ++i) {
      line[n++] = ' ';
      line[n++] = "0123456789abcdef"[program[i] >> 4];
      line[n++] = "0123456789abcdef"[program[i] & 15];
    }
    line[n++] = '\n';
    line[n] = 0;
    Log(line);
  }
}

// The game's imports whose names have to do with time, waiting or the window, with where their
// pointers are, to read the dumped code's indirect calls.
static void LogGameImports(const unsigned char* program) {
  static const char* const words[] = {"Sleep", "Time", "Tick", "Perf", "Wait", "Yield", "Thread", "Window",
                                      "Focus", "Foreground", "Iconic", "Message", "Timer", NULL};
  const unsigned char* nt = program + *(const int32_t*)(program + 0x3c);
  uint32_t directory = *(const uint32_t*)(nt + 24 + 120);
  if (!directory) return;
  for (const uint32_t* descriptor = (const uint32_t*)(program + directory); descriptor[3]; descriptor += 5) {
    if (!descriptor[0]) continue;
    const uint64_t* names = (const uint64_t*)(program + descriptor[0]);
    for (size_t i = 0; names[i]; ++i) {
      if (names[i] >> 63) continue;
      const char* name = (const char*)(program + (uint32_t)names[i] + 2);
      BOOL wanted = 0;
      for (int w = 0; words[w] && !wanted; ++w)
        for (const char* p = name; *p && !wanted; ++p) {
          size_t k = 0;
          while (words[w][k] && p[k] == words[w][k]) ++k;
          wanted = !words[w][k];
        }
      if (!wanted) continue;
      char line[200];
      size_t n = SampleAppend(line, 0, sizeof(line), "libcef-shim: import ");
      n = SampleAppend(line, n, sizeof(line), (const char*)(program + descriptor[3]));
      n = SampleAppend(line, n, sizeof(line), "!");
      n = SampleAppend(line, n, sizeof(line), name);
      n = SampleAppend(line, n, sizeof(line), " at ");
      n = HitchHex(line, n, sizeof(line), descriptor[4] + i * 8);
      SampleAppend(line, n, sizeof(line), "\n");
      Log(line);
    }
  }
}

static DWORD WINAPI SleepWatch(void* unused) {
  (void)unused;
  // BeamNG 0.40.4.0 (build 296339): where the main thread waited in build 23's stalls, and its callers.
  static const uint32_t waited[] = {0x1ca128f, 0xab1bb4, 0x65f5de, 0xf9602e, 0x2c1ac98};
  const unsigned char* program = (const unsigned char*)GetModuleHandleW(NULL);
  Sleep(30000);  // after start-up, so the log reads easily
  for (int i = 0; i < 5; ++i) DumpGameFunction(program, waited[i]);
  LogGameImports(program);
  SleepSummary(0, 0);  // the counts so far, from start-up; then one line every 10 s
  long watched = 0, late_logged = 0;
  int64_t longest = 0, summary_at = 0;
  for (;;) {
    int64_t before = 0, after = 0;
    uint64_t tick_before = GetTickCount64();
    RealQpc(&before);
    Sleep(2);
    RealQpc(&after);
    uint64_t tick_after = GetTickCount64();
    int64_t took = after - before;
    ++watched;
    if (took > longest) longest = took;
    int64_t ms = took * 1000 / g_qpc_frequency, tick_ms = (int64_t)(tick_after - tick_before);
    if ((ms > 50 || tick_ms - ms > 50 || ms - tick_ms > 50) && late_logged < 40) {
      ++late_logged;
      char line[200];
      size_t n = SampleAppend(line, 0, sizeof(line), "libcef-shim: sleep: the shim's 2 ms sleep took ");
      n = SampleNumber(line, n, sizeof(line), (uint64_t)ms);
      n = SampleAppend(line, n, sizeof(line), " ms by QueryPerformanceCounter, ");
      n = SampleNumber(line, n, sizeof(line), (uint64_t)(tick_ms > 0 ? tick_ms : 0));
      n = SampleAppend(line, n, sizeof(line), " ms by GetTickCount64, at ");
      n = SampleNumber(line, n, sizeof(line), (uint64_t)((before - g_start_qpc) * 10 / g_qpc_frequency / 10));
      n = SampleAppend(line, n, sizeof(line), ".");
      n = SampleNumber(line, n, sizeof(line), (uint64_t)((before - g_start_qpc) * 10 / g_qpc_frequency % 10));
      SampleAppend(line, n, sizeof(line), " s\n");
      Log(line);
    }
    if (!summary_at) summary_at = after;
    if (after - summary_at >= 10 * g_qpc_frequency) {
      SleepSummary(watched, longest);
      summary_at = after;
      watched = 0;
      longest = 0;
    }
  }
  return 0;
}

// From DllMain: BeamNG's Sleep and QueryPerformanceCounter imports go through the shim.
static void HookGameTime(void) {
  QueryPerformanceFrequency(&g_qpc_frequency);
  if (!g_qpc_frequency) return;
  HMODULE program = GetModuleHandleW(NULL);
  g_real_qpc = (qpc_fn)PatchImport(program, "QueryPerformanceCounter", (void*)GameQpc);
  g_real_sleep = (sleep_fn)PatchImport(program, "Sleep", (void*)GameSleep);
  Log(g_real_sleep ? (g_real_sleep_only ? "libcef-shim: sleep: the game's Sleep is watched\n"
                                        : "libcef-shim: sleep: the game's Sleep is watched; the main thread's "
                                          "short sleeps yield instead\n")
                   : "libcef-shim: sleep: the game imports no Sleep\n");
  if (!g_real_qpc) Log("libcef-shim: sleep: the game imports no QueryPerformanceCounter\n");
  CreateThread(NULL, 0, (void*)SleepWatch, NULL, 0, NULL);
}

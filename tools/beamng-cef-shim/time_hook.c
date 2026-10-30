// SPDX-License-Identifier: GPL-3.0-or-later
//
// The game's sleeps and clock reads, included by shim.c.
//
// In each stall build 23 caught on the console, BeamNG's main thread was in Sleep, or reading
// QueryPerformanceCounter, in std::this_thread::sleep_for, its wait between frames. Build 24 found
// the game's sleeps on time (about 11 ms asked, 0.1 ms more taken) and GetTickCount64 standing
// still for about 212 ms in each stall while QueryPerformanceCounter went on: the server's thread,
// which keeps that tick, was busy (watch_hook.c). What this keeps measuring:
// - BeamNG.drive.x64.exe's Sleep import is wrapped. Every 10 s a line counts the main thread's
//   calls, how long they asked for and took, and the longest, with where it came from; the other
//   threads' calls are counted. A main-thread Sleep that took over 50 ms more than it asked for is
//   logged as it happens;
// - a thread of the shim's own sleeps 2 ms at a time and logs each sleep that took over 50 ms, and
//   when GetTickCount64 and QueryPerformanceCounter disagree by as much; the 10 s line counts the
//   times GetTickCount64 stood still that long;
// - with shim:yield-sleep, the main thread's short sleeps (1 to 4 ms) wait by yielding the CPU
//   until the time has come, which needs no timer;
// - its QueryPerformanceCounter import is counted, as a call takes about 1 us on the console.

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
  if (ms && ms <= 4 && g_yield_sleep) {
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

// Every 10 s: the game's sleeps and clock reads, and how often GetTickCount64 stood still.
static void SleepSummary(long watched, int64_t watch_longest, long tick_stalls, int64_t tick_longest) {
  static char line[700];
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
  n = SampleAppend(line, n, sizeof(line), " ms; GetTickCount64 stood still ");
  n = SampleNumber(line, n, sizeof(line), (uint64_t)tick_stalls);
  n = SampleAppend(line, n, sizeof(line), " times for over 50 ms");
  if (tick_stalls) {
    n = SampleAppend(line, n, sizeof(line), ", the longest ");
    n = SampleNumber(line, n, sizeof(line), (uint64_t)tick_longest);
    n = SampleAppend(line, n, sizeof(line), " ms");
  }
  SampleAppend(line, n, sizeof(line), "\n");
  Log(line);
}

static DWORD WINAPI SleepWatch(void* unused) {
  (void)unused;
  Sleep(30000);  // after start-up, so the log reads easily
  SleepSummary(0, 0, 0, 0);  // the counts so far, from start-up; then one line every 10 s
  long watched = 0, late_logged = 0, tick_stalls = 0;
  int64_t longest = 0, summary_at = 0, tick_longest = 0;
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
    if (tick_ms - ms > 50) {
      ++tick_stalls;
      if (tick_ms - ms > tick_longest) tick_longest = tick_ms - ms;
    }
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
      SleepSummary(watched, longest, tick_stalls, tick_longest);
      summary_at = after;
      watched = tick_stalls = 0;
      longest = tick_longest = 0;
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
  Log(g_real_sleep ? (g_yield_sleep ? "libcef-shim: sleep: the game's Sleep is watched; the main thread's short "
                                      "sleeps yield instead\n"
                                    : "libcef-shim: sleep: the game's Sleep is watched\n")
                   : "libcef-shim: sleep: the game imports no Sleep\n");
  if (!g_real_qpc) Log("libcef-shim: sleep: the game imports no QueryPerformanceCounter\n");
  CreateThread(NULL, 0, (void*)SleepWatch, NULL, 0, NULL);
}

// SPDX-License-Identifier: GPL-3.0-or-later
//
// What the game's threads are doing when a frame is late, included by shim.c.
//
// On the console BeamNG stops for about 250 ms every 1.7 s, in menus and while driving, and the
// thread that presents just waits through it (build 22). A watchdog checks every 20 ms how long ago
// the last frame was presented. Once a frame is 120 ms late in smooth play (40 frames a second over
// the 3 s before, from a minute after the shim loaded), it stops every thread of the game at once,
// copies where each is and the code addresses on the top of its stack, lets them all go, and logs a
// line per thread: its name (Chromium names its threads, and many games do), where it was, with the
// function for the modules of Windows and Wine, and the code addresses on its stack (a scan, so some
// are stale). Stopping them together shows one moment of the stall: a stop takes 11 to 15 ms to land
// on the console, one at a time over a hundred threads would take seconds. Nothing that takes a lock
// runs while they are stopped. It lists six stalls, 5 s apart at least. It also times
// cef_do_message_loop_work, which the main thread (CEF's UI thread, the one that called
// cef_initialize) calls each frame, says whether the main thread was inside it, and logs every 10 s
// how long the calls took. shim:no-hitch turns it off.
//
// The frames of 34 to 100 ms left in build 27 came at a steady rhythm: one of about 36 ms with each
// call into CEF of about 25 ms, every 5.01 s, and two of about 49 ms in the same 5.01 s, 2 s apart,
// outside CEF. So the watchdog also stops the main thread and the ones that submit and present
// when a frame is 30 ms late in smooth play, and lists them the same way as "slow frame" lines:
// twelve frames, 15 s apart at least. Each stop holds those threads about as long as it takes to
// land, which makes those twelve frames that much longer.

enum {
  kHitchThreads = 192,
  kHitchWords = 96,
  kHitchShown = 10,  // code addresses per thread
  kHitchLists = 6,
  kHitchSamples = 12,  // slow frames
};

typedef struct {
  DWORD id;
  HANDLE handle;
  const volatile uint64_t* teb;
  BOOL stopped;
  uint64_t rip;
  uint32_t count;
  uint64_t words[kHitchWords];
  char name[48];
} hitch_thread_t;

static hitch_thread_t g_hitch_threads[kHitchThreads];
static volatile int64_t g_cef_work_since;    // when the current cef_do_message_loop_work began; 0 outside it
static volatile int64_t g_cef_work_longest;  // this 10 s
static volatile long g_cef_work_calls;
static volatile int64_t g_cef_work_total;
// Calls over 8 ms this 10 s: when they began, since the shim loaded, and how long; to set beside the
// pace line's slow frames.
static int64_t g_cef_slow_at[12], g_cef_slow_length[12];
static volatile long g_cef_slows;

typedef void (*cef_do_message_loop_work_fn)(void);

__declspec(dllexport) void cef_do_message_loop_work(void) {
  static cef_do_message_loop_work_fn real;
  if (!real) real = (cef_do_message_loop_work_fn)GetProcAddress(RealLibrary(), "cef_do_message_loop_work");
  if (!real) return;
  int64_t start = PaceNow();
  g_cef_work_since = start;
  real();
  int64_t spent = PaceNow() - start;
  g_cef_work_since = 0;
  ++g_cef_work_calls;
  g_cef_work_total += spent;
  if (spent > g_cef_work_longest) g_cef_work_longest = spent;
  if (g_pace.frequency && spent * 125 > g_pace.frequency && g_cef_slows < 12) {
    g_cef_slow_at[g_cef_slows] = start - g_start_qpc;
    g_cef_slow_length[g_cef_slows] = spent;
    ++g_cef_slows;
  }
}

static size_t HitchHex(char* line, size_t n, size_t capacity, uint64_t value) {
  char digits[20];
  int d = 0;
  do digits[d++] = "0123456789abcdef"[value & 15], value >>= 4; while (value && d < 20);
  n = SampleAppend(line, n, capacity, "0x");
  while (d && n + 1 < capacity) line[n++] = digits[--d];
  line[n] = 0;
  return n;
}

// "module+0xoffset", with "(export)" for the modules of Windows and Wine, or the bare address.
static size_t HitchCode(char* line, size_t n, size_t capacity, uint64_t address) {
  const sample_module_t* module = SampleModule(address, 1);
  if (!module) return HitchHex(line, n, capacity, address);
  n = SampleAppend(line, n, capacity, module->name);
  n = SampleAppend(line, n, capacity, "+");
  n = HitchHex(line, n, capacity, address - (uint64_t)(uintptr_t)module->module);
  const char* name = module->system ? NearestExport(module->module, address) : NULL;
  if (name) {
    n = SampleAppend(line, n, capacity, "(");
    n = SampleAppend(line, n, capacity, name);
    n = SampleAppend(line, n, capacity, ")");
  }
  return n;
}

typedef long(WINAPI* hitch_query_fn)(HANDLE, int, void*, DWORD, DWORD*);
typedef long(WINAPI* hitch_describe_fn)(HANDLE, WCHAR**);
typedef void*(WINAPI* hitch_free_fn)(void*);

// Opens thread id and notes its name and TEB, while it runs.
static BOOL HitchOpen(hitch_thread_t* t, DWORD id, hitch_query_fn query, hitch_describe_fn describe,
                      hitch_free_fn free_name) {
  t->id = id;
  t->teb = NULL;
  t->stopped = 0;
  t->rip = 0;
  t->count = 0;
  t->name[0] = 0;
  t->handle = OpenThread(0x004a /* SUSPEND_RESUME | GET_CONTEXT | QUERY_INFORMATION */, 0, id);
  if (!t->handle) return 0;
  WCHAR* name = NULL;
  if (describe && describe(t->handle, &name) >= 0 && name) {
    size_t n = 0;
    for (; name[n] && n + 1 < sizeof(t->name); ++n) t->name[n] = name[n] < 0x80 ? (char)name[n] : '?';
    t->name[n] = 0;
    if (free_name) free_name(name);
  }
  struct {
    long exit_status;
    void* teb;
    uint64_t client_id[2];
    uint64_t affinity;
    long priority, base_priority;
  } info;
  if (query && query(t->handle, 0 /* ThreadBasicInformation */, &info, sizeof(info), NULL) == 0)
    t->teb = (const volatile uint64_t*)info.teb;
  return 1;
}

// Copies where a stopped thread is: its instruction pointer and the top of its stack, between the
// limits its TEB gives.
static void HitchCopy(hitch_thread_t* t) {
  static _Alignas(16) unsigned char context[1232];
  *(DWORD*)(context + 0x30) = 0x100001;  // CONTEXT_CONTROL
  *(uint64_t*)(context + CONTEXT_RIP) = 0;
  *(uint64_t*)(context + CONTEXT_RSP) = 0;
  if (!GetThreadContext(t->handle, context)) return;
  t->rip = *(uint64_t*)(context + CONTEXT_RIP);
  uint64_t rsp = *(uint64_t*)(context + CONTEXT_RSP);
  uint64_t stack_base = t->teb ? t->teb[1] : 0, stack_limit = t->teb ? t->teb[2] : 0;  // NT_TIB
  if (!t->teb || rsp < stack_limit || rsp >= stack_base) return;
  uint32_t count = (uint32_t)((stack_base - rsp) / 8);
  if (count > kHitchWords) count = kHitchWords;
  for (uint32_t i = 0; i < count; ++i) t->words[i] = ((const volatile uint64_t*)(uintptr_t)rsp)[i];
  t->count = count;
}

// The main thread, the one that submits and the one that presents, each once.
static int HitchKeyThreads(DWORD* ids) {
  DWORD key[3] = {g_main_thread, g_vkt.thread, g_vkt.presenter};
  int count = 0;
  for (int i = 0; i < 3; ++i) {
    BOOL seen = !key[i];
    for (int k = 0; k < count && !seen; ++k) seen = ids[k] == key[i];
    if (!seen) ids[count++] = key[i];
  }
  return count;
}

// The process's threads: the main thread, the one that submits and the one that presents first.
static int HitchThreadIds(DWORD* ids, int capacity) {
  typedef struct { DWORD size, usage, thread_id, process_id; long base_priority, delta_priority; DWORD flags; } entry_t;
  DWORD self = GetCurrentThreadId(), process = GetCurrentProcessId();
  int count = HitchKeyThreads(ids);
  HANDLE snapshot = CreateToolhelp32Snapshot(4 /* TH32CS_SNAPTHREAD */, 0);
  if (snapshot == INVALID_HANDLE_VALUE) return count;
  entry_t entry;
  entry.size = sizeof(entry);
  for (BOOL more = Thread32First(snapshot, &entry); more && count < capacity;
       entry.size = sizeof(entry), more = Thread32Next(snapshot, &entry)) {
    if (entry.process_id != process || entry.thread_id == self) continue;
    BOOL seen = 0;
    for (int k = 0; k < 3 && k < count && !seen; ++k) seen = ids[k] == entry.thread_id;
    if (!seen) ids[count++] = entry.thread_id;
  }
  CloseHandle(snapshot);
  return count;
}

// One line for the list ("hitch" or "slow frame"), then one per thread. late, at and in_cef_work are
// as the watchdog found them, before it stopped any thread.
static void HitchReport(const char* what, int list, int64_t late, int64_t at, int64_t in_cef_work, int64_t held,
                        const hitch_thread_t* threads, int count, int64_t frequency) {
  static char line[2048];
  SampleModulesRefresh();
  size_t n = SampleAppend(line, 0, sizeof(line), "libcef-shim: ");
  n = SampleAppend(line, n, sizeof(line), what);
  n = SampleAppend(line, n, sizeof(line), " ");
  n = SampleNumber(line, n, sizeof(line), (uint64_t)list);
  n = SampleAppend(line, n, sizeof(line), ": no frame for ");
  n = SampleNumber(line, n, sizeof(line), (uint64_t)(late * 1000 / frequency));
  n = SampleAppend(line, n, sizeof(line), " ms at ");
  n = SampleNumber(line, n, sizeof(line), (uint64_t)((at - g_start_qpc) * 10 / frequency / 10));
  n = SampleAppend(line, n, sizeof(line), ".");
  n = SampleNumber(line, n, sizeof(line), (uint64_t)((at - g_start_qpc) * 10 / frequency % 10));
  n = SampleAppend(line, n, sizeof(line), " s; ");
  n = SampleNumber(line, n, sizeof(line), (uint64_t)count);
  n = SampleAppend(line, n, sizeof(line), " threads, held for ");
  n = SampleNumber(line, n, sizeof(line), (uint64_t)(held * 1000 / frequency));
  n = SampleAppend(line, n, sizeof(line), " ms");
  n = SampleAppend(line, n, sizeof(line),
                   in_cef_work ? "; the main thread was in cef_do_message_loop_work, for "
                               : "; the main thread was not in cef_do_message_loop_work");
  if (in_cef_work) {
    n = SampleNumber(line, n, sizeof(line), (uint64_t)(in_cef_work * 1000 / frequency));
    n = SampleAppend(line, n, sizeof(line), " ms");
  }
  SampleAppend(line, n, sizeof(line), "\n");
  Log(line);
  for (int i = 0; i < count; ++i) {
    const hitch_thread_t* t = &threads[i];
    n = SampleAppend(line, 0, sizeof(line), "libcef-shim:   thread ");
    n = HitchHex(line, n, sizeof(line), t->id);
    if (t->name[0]) {
      n = SampleAppend(line, n, sizeof(line), " '");
      n = SampleAppend(line, n, sizeof(line), t->name);
      n = SampleAppend(line, n, sizeof(line), "'");
    }
    if (t->id == g_main_thread) n = SampleAppend(line, n, sizeof(line), " (main)");
    if (t->id == g_vkt.thread) n = SampleAppend(line, n, sizeof(line), " (submits)");
    if (t->id == g_vkt.presenter) n = SampleAppend(line, n, sizeof(line), " (presents)");
    if (!t->rip) {
      SampleAppend(line, n, sizeof(line), " not stopped\n");
      Log(line);
      continue;
    }
    n = SampleAppend(line, n, sizeof(line), " at ");
    n = HitchCode(line, n, sizeof(line), t->rip);
    int shown = 0;
    for (uint32_t k = 0; k < t->count && shown < kHitchShown && n + 200 < sizeof(line); ++k) {
      if (!SampleModule(t->words[k], 0)) continue;
      n = SampleAppend(line, n, sizeof(line), " < ");
      n = HitchCode(line, n, sizeof(line), t->words[k]);
      ++shown;
    }
    SampleAppend(line, n, sizeof(line), "\n");
    Log(line);
  }
}

static DWORD WINAPI HitchWatch(void* unused) {
  (void)unused;
  static DWORD ids[kHitchThreads];
  static const WCHAR ntdll_name[] = {'n', 't', 'd', 'l', 'l', '.', 'd', 'l', 'l', 0};
  static const WCHAR kernelbase_name[] = {'k', 'e', 'r', 'n', 'e', 'l', 'b', 'a', 's', 'e', '.', 'd', 'l', 'l', 0};
  static const WCHAR kernel32_name[] = {'k', 'e', 'r', 'n', 'e', 'l', '3', '2', '.', 'd', 'l', 'l', 0};
  HMODULE ntdll = GetModuleHandleW(ntdll_name), kernelbase = GetModuleHandleW(kernelbase_name);
  HMODULE kernel32 = GetModuleHandleW(kernel32_name);
  hitch_query_fn query = ntdll ? (hitch_query_fn)GetProcAddress(ntdll, "NtQueryInformationThread") : NULL;
  hitch_describe_fn describe =
      kernelbase ? (hitch_describe_fn)GetProcAddress(kernelbase, "GetThreadDescription") : NULL;
  if (!describe && kernel32) describe = (hitch_describe_fn)GetProcAddress(kernel32, "GetThreadDescription");
  hitch_free_fn free_name = kernel32 ? (hitch_free_fn)GetProcAddress(kernel32, "LocalFree") : NULL;
  int64_t frequency = 0;
  QueryPerformanceFrequency(&frequency);
  if (!frequency) return 0;
  // Presents counted once a second, for the frame rate of the last 3 s.
  long presents_at[4] = {0, 0, 0, 0};
  int64_t second = 0, last_list = 0, listed_present = 0, last_sample = 0, sampled_present = 0, stats_at = 0;
  int lists = 0, samples = 0, seconds = 0;
  for (;;) {
    Sleep(20);
    int64_t now = PaceNow();
    if (!stats_at) stats_at = now;
    if (now - stats_at >= 10 * frequency) {
      long calls = g_cef_work_calls;
      int64_t longest = g_cef_work_longest, total = g_cef_work_total;
      g_cef_work_calls = 0;
      g_cef_work_longest = g_cef_work_total = 0;
      stats_at = now;
      if (calls) {
        char line[200];
        size_t n = SampleAppend(line, 0, sizeof(line), "libcef-shim: cef_do_message_loop_work: ");
        n = SampleNumber(line, n, sizeof(line), (uint64_t)calls);
        n = SampleAppend(line, n, sizeof(line), " calls in 10 s, ");
        n = SampleNumber(line, n, sizeof(line), (uint64_t)(total * 1000000 / frequency / calls));
        n = SampleAppend(line, n, sizeof(line), " us each, longest ");
        n = SampleNumber(line, n, sizeof(line), (uint64_t)(longest * 1000 / frequency));
        SampleAppend(line, n, sizeof(line), " ms\n");
        Log(line);
      }
      LogCefViews();
      LogMemoryChecks();
      long slows = g_cef_slows;
      PaceList("cef_do_message_loop_work over 8 ms", slows > 12 ? 12 : (int)slows, g_cef_slow_at, g_cef_slow_length);
      g_cef_slows = 0;
    }
    if (!g_pace.frequency || (lists >= kHitchLists && samples >= kHitchSamples)) continue;
    if (now - second >= frequency) {
      second = now;
      for (int i = 0; i < 3; ++i) presents_at[i] = presents_at[i + 1];
      presents_at[3] = g_presents;
      ++seconds;
    }
    int64_t last = g_pace.last, late = now - last;
    if (seconds < 4 || now - g_start_qpc < 60 * frequency) continue;
    if (presents_at[2] - presents_at[0] < 80) continue;  // 40 a second before it: smooth play, not loading
    // A stall: every thread, 5 s apart. A slow frame: the main thread and the ones that submit and
    // present, 15 s apart.
    BOOL stall = lists < kHitchLists && late >= frequency * 120 / 1000 && last != listed_present &&
                 (!last_list || now - last_list >= 5 * frequency);
    BOOL slow = !stall && samples < kHitchSamples && late >= frequency * 30 / 1000 && late < frequency * 120 / 1000 &&
                last != sampled_present && (!last_sample || now - last_sample >= 15 * frequency);
    if (!stall && !slow) continue;
    if (stall) listed_present = last, last_list = now;
    else sampled_present = last, last_sample = now;
    int64_t since = g_cef_work_since, in_cef_work = since ? now - since : 0;
    int count = stall ? HitchThreadIds(ids, kHitchThreads) : HitchKeyThreads(ids), taken = 0;
    for (int i = 0; i < count; ++i)
      if (HitchOpen(&g_hitch_threads[taken], ids[i], query, describe, free_name)) ++taken;
    int64_t stop_from = PaceNow();
    for (int i = 0; i < taken; ++i)
      g_hitch_threads[i].stopped = SuspendThread(g_hitch_threads[i].handle) != (DWORD)-1;
    for (int i = 0; i < taken; ++i)
      if (g_hitch_threads[i].stopped) HitchCopy(&g_hitch_threads[i]);
    for (int i = 0; i < taken; ++i)
      if (g_hitch_threads[i].stopped) ResumeThread(g_hitch_threads[i].handle);
    int64_t held = PaceNow() - stop_from;
    for (int i = 0; i < taken; ++i) CloseHandle(g_hitch_threads[i].handle);
    if (stall) HitchReport("hitch", ++lists, late, now, in_cef_work, held, g_hitch_threads, taken, frequency);
    else HitchReport("slow frame", ++samples, late, now, in_cef_work, held, g_hitch_threads, taken, frequency);
  }
  return 0;
}

// SPDX-License-Identifier: GPL-3.0-or-later
//
// Chromium's and BeamNG's checks of memory, included by shim.c after sampler.c.
//
// Every 5 seconds Chromium's memory pressure check (SystemMemoryPressureEvaluator) asks Windows how
// much memory is free (GlobalMemoryStatusEx) on its browser thread, which in BeamNG is the game's
// main thread, inside cef_do_message_loop_work. On the console that call takes 24 to 25 ms (build
// 29): it asks for the process's memory counters (NtQueryInformationProcess, ProcessVmCounters),
// which Wine works out there from every mapping. In build 27 a call into CEF of about 25 ms came
// every 5.01 s, and with it 332 of the 334 frames of 34 to 40 ms; with build 29's cache they were
// gone. BeamNG's main thread asks for the same counters (GetProcessMemoryInfo): build 29 found it
// there in one of the frames of about 50 ms that still came every 2 s.
//
// So the shim answers these calls with Windows' last answer while it is under a minute old:
// GlobalMemoryStatusEx from libcef_real.dll, BeamNG.drive.x64.exe and mimalloc.dll, and
// GetProcessMemoryInfo and K32GetProcessMemoryInfo for the game's own process from the same three.
// It times each call it passes on, logs the first of each kind with what Windows said and any that
// took over 5 ms, and every 10 s how many calls there were. shim:no-memory-cache passes every call
// on.

typedef BOOL(WINAPI* memory_status_fn)(void* status);
typedef BOOL(WINAPI* process_memory_fn)(HANDLE process, void* counters, DWORD size);

// One cached answer: its bytes, how many, and when Windows gave it.
typedef struct {
  volatile long lock;
  unsigned char bytes[96];
  DWORD size;
  int64_t at;
  volatile long calls, passed, logged;
} memory_answer_t;

static memory_answer_t g_memory_status_answer, g_process_memory_answer;
static memory_status_fn g_real_memory_status;
static process_memory_fn g_real_process_memory, g_real_process_memory_k32;

static int64_t MemoryFrequency(void) {
  static int64_t frequency;
  if (!frequency) QueryPerformanceFrequency(&frequency);
  return frequency;
}

static void MemoryLock(memory_answer_t* a) {
  while (__sync_lock_test_and_set(&a->lock, 1)) Sleep(0);
}
static void MemoryUnlock(memory_answer_t* a) { __sync_lock_release(&a->lock); }

static void MemoryCopy(unsigned char* to, const unsigned char* from, DWORD size) {
  for (DWORD i = 0; i < size; ++i) to[i] = from[i];
}

// Copies a fresh answer of this size into to; 0 when there is none.
static BOOL MemoryCached(memory_answer_t* a, void* to, DWORD size, int64_t now) {
  if (g_no_memory_cache) return 0;
  MemoryLock(a);
  BOOL fresh = a->at && a->size == size && now - a->at < 60 * MemoryFrequency();
  if (fresh) MemoryCopy(to, a->bytes, size);
  MemoryUnlock(a);
  return fresh;
}

static void MemoryKeep(memory_answer_t* a, const void* from, DWORD size) {
  MemoryLock(a);
  MemoryCopy(a->bytes, from, size);
  a->size = size;
  a->at = PaceNow();
  MemoryUnlock(a);
}

static BOOL MemoryShouldLog(memory_answer_t* a, int64_t took) {
  long logged = a->logged;
  if (logged && (took * 200 <= MemoryFrequency() || logged >= 12)) return 0;
  __sync_add_and_fetch(&a->logged, 1);
  return 1;
}

static size_t MemoryMiB(char* line, size_t n, size_t capacity, const char* text, uint64_t bytes) {
  n = SampleAppend(line, n, capacity, text);
  n = SampleNumber(line, n, capacity, bytes >> 20);
  return SampleAppend(line, n, capacity, " MiB");
}

// MEMORYSTATUSEX: dwLength, dwMemoryLoad, then the total and free bytes of physical memory, the
// page file and the virtual address space.
static void LogMemoryStatus(const unsigned char* status, int64_t took) {
  static const char* const kinds[] = {"; physical ", "; page file ", "; virtual "};
  const uint64_t* bytes = (const uint64_t*)(status + 8);
  char line[300];
  size_t n = SampleAppend(line, 0, sizeof(line), "libcef-shim: memory: GlobalMemoryStatusEx took ");
  n = SampleNumber(line, n, sizeof(line), (uint64_t)(took * 1000 / MemoryFrequency()));
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

// PROCESS_MEMORY_COUNTERS(_EX): cb, PageFaultCount, then the peak and current working set at 8 and
// 16, the paged and non-paged pool, the page file use and its peak at 56 and 64, and in the _EX the
// private bytes at 72.
static void LogProcessMemory(const unsigned char* counters, DWORD size, int64_t took) {
  char line[300];
  size_t n = SampleAppend(line, 0, sizeof(line), "libcef-shim: memory: GetProcessMemoryInfo took ");
  n = SampleNumber(line, n, sizeof(line), (uint64_t)(took * 1000 / MemoryFrequency()));
  n = SampleAppend(line, n, sizeof(line), " ms: working set");
  n = MemoryMiB(line, n, sizeof(line), " ", *(const uint64_t*)(counters + 16));
  n = MemoryMiB(line, n, sizeof(line), ", at most ", *(const uint64_t*)(counters + 8));
  n = MemoryMiB(line, n, sizeof(line), "; committed ", *(const uint64_t*)(counters + 56));
  if (size >= 80) n = MemoryMiB(line, n, sizeof(line), "; private ", *(const uint64_t*)(counters + 72));
  SampleAppend(line, n, sizeof(line), "\n");
  Log(line);
}

static BOOL WINAPI MemoryStatusHook(void* status) {
  memory_answer_t* a = &g_memory_status_answer;
  if (!status || *(DWORD*)status != 64 || !MemoryFrequency()) return g_real_memory_status(status);
  int64_t now = PaceNow();
  __sync_add_and_fetch(&a->calls, 1);
  if (MemoryCached(a, status, 64, now)) return 1;
  BOOL answered = g_real_memory_status(status);
  int64_t took = PaceNow() - now;
  __sync_add_and_fetch(&a->passed, 1);
  if (!answered) return answered;
  MemoryKeep(a, status, 64);
  if (MemoryShouldLog(a, took)) LogMemoryStatus(status, took);
  return answered;
}

// Only the game's own process (GetCurrentProcess), at the sizes of the two structs and of _EX2.
static BOOL ProcessMemory(process_memory_fn real, HANDLE process, void* counters, DWORD size) {
  memory_answer_t* a = &g_process_memory_answer;
  if (process != (HANDLE)(intptr_t)-1 || !counters || size < 72 || size > 96 || !MemoryFrequency())
    return real(process, counters, size);
  int64_t now = PaceNow();
  __sync_add_and_fetch(&a->calls, 1);
  if (MemoryCached(a, counters, size, now)) return 1;
  BOOL answered = real(process, counters, size);
  int64_t took = PaceNow() - now;
  __sync_add_and_fetch(&a->passed, 1);
  if (!answered) return answered;
  MemoryKeep(a, counters, size);
  if (MemoryShouldLog(a, took)) LogProcessMemory(counters, size, took);
  return answered;
}

static BOOL WINAPI ProcessMemoryHook(HANDLE process, void* counters, DWORD size) {
  return ProcessMemory(g_real_process_memory, process, counters, size);
}
static BOOL WINAPI ProcessMemoryK32Hook(HANDLE process, void* counters, DWORD size) {
  return ProcessMemory(g_real_process_memory_k32, process, counters, size);
}

// Patches one module's imports. The first module that imports a function gives its original; one
// already patched gives the hook back, which is not kept.
static void HookMemoryImports(HMODULE module) {
  if (!module) return;
  void* status = PatchImport(module, "GlobalMemoryStatusEx", (void*)MemoryStatusHook);
  void* process = PatchImport(module, "GetProcessMemoryInfo", (void*)ProcessMemoryHook);
  void* k32 = PatchImport(module, "K32GetProcessMemoryInfo", (void*)ProcessMemoryK32Hook);
  if (status && status != (void*)MemoryStatusHook && !g_real_memory_status)
    g_real_memory_status = (memory_status_fn)status;
  if (process && process != (void*)ProcessMemoryHook && !g_real_process_memory)
    g_real_process_memory = (process_memory_fn)process;
  if (k32 && k32 != (void*)ProcessMemoryK32Hook && !g_real_process_memory_k32)
    g_real_process_memory_k32 = (process_memory_fn)k32;
}

// From cef_initialize, once libcef_real.dll is loaded.
static void HookMemoryCheck(HMODULE real) {
  static const WCHAR mimalloc[] = {'m', 'i', 'm', 'a', 'l', 'l', 'o', 'c', '.', 'd', 'l', 'l', 0};
  HookMemoryImports(real);
  HookMemoryImports(GetModuleHandleW(NULL));
  HookMemoryImports(GetModuleHandleW(mimalloc));
  VkLog("memory: hooked GlobalMemoryStatusEx, GetProcessMemoryInfo, K32GetProcessMemoryInfo",
        g_real_memory_status != NULL, g_real_process_memory != NULL, g_real_process_memory_k32 != NULL);
  Log(g_no_memory_cache ? "libcef-shim: memory: those calls are timed and passed on\n"
                        : "libcef-shim: memory: those calls get answers under a minute old\n");
}

// Every 10 s, from the watchdog's thread.
static void LogMemoryChecks(void) {
  memory_answer_t* answers[2] = {&g_memory_status_answer, &g_process_memory_answer};
  long calls[2], passed[2];
  for (int i = 0; i < 2; ++i)
    calls[i] = __sync_lock_test_and_set(&answers[i]->calls, 0),
    passed[i] = __sync_lock_test_and_set(&answers[i]->passed, 0);
  if (!calls[0] && !calls[1]) return;
  char line[200];
  size_t n = SampleAppend(line, 0, sizeof(line), "libcef-shim: memory: in 10 s, ");
  n = SampleNumber(line, n, sizeof(line), (uint64_t)calls[0]);
  n = SampleAppend(line, n, sizeof(line), " checks of free memory (");
  n = SampleNumber(line, n, sizeof(line), (uint64_t)passed[0]);
  n = SampleAppend(line, n, sizeof(line), " passed on to Windows) and ");
  n = SampleNumber(line, n, sizeof(line), (uint64_t)calls[1]);
  n = SampleAppend(line, n, sizeof(line), " of the game's memory use (");
  n = SampleNumber(line, n, sizeof(line), (uint64_t)passed[1]);
  SampleAppend(line, n, sizeof(line), " passed on)\n");
  Log(line);
}

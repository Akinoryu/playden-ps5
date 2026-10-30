// SPDX-License-Identifier: GPL-3.0-or-later
//
// Where the thread that submits spends its time, included by shim.c; off unless shim:sample is set.
//
// Ten times a second for 30 s of every minute, the sampler stops that thread (the one the pace
// lines time), copies its instruction pointer and the top of its stack, and lets it go; nothing
// that takes a lock runs while it is stopped. A stop held the thread 0.9 ms on average under Wine
// on a PC, but 11 to 15 ms on the console, up to 387 ms (build 21, on the thread that presents,
// which mostly waits), so it is off by default; the pace lines of the other 30 s are free of it.
//
// Then the sample is named: the module it was in, with the nearest export for the modules of
// Windows and Wine (ntdll.dll!NtWaitForSingleObject), or the Vulkan call when it was in one (in
// the shim's dispatcher, called from winevulkan's thunk for that call); outside any module, code
// a program generated (LuaJIT's) or Wine's host side. Samples in Windows, Wine, Vulkan or outside
// any module are also counted by the first game module on the stack, the one that made the call.
// After each 30 s the most frequent names are logged, with how long the stops held the thread.

enum {
  kSampleWords = 256,
  kSampleKeys = 96,
  kSampleKey = 72,
  kSampleModules = 256,
  kSampleTop = 12,
  kSamplePeriodMs = 100,
  kSampleOnSeconds = 30,
  kSampleCycleSeconds = 60,
};

typedef struct {
  char key[kSampleKey];
  long count;
} sample_bucket_t;

typedef struct {
  HMODULE module;
  uint64_t code_start, code_end;  // its executable sections
  BOOL system;                    // under C:\windows (Wine's DLLs), or the shim: looked past for the caller
  BOOL vulkan;                    // winevulkan.dll
  BOOL shim;
  char name[40];
} sample_module_t;

typedef struct {
  sample_bucket_t buckets[kSampleKeys];
  int used;
  long lost;  // samples whose name found no room
} sample_table_t;

static sample_table_t g_where, g_from;
static sample_module_t g_sample_modules[kSampleModules];
static int g_sample_module_count;

static BOOL SameText(const char* a, const char* b) {
  for (; *a && *b; ++a, ++b) {
    char x = *a >= 'A' && *a <= 'Z' ? (char)(*a + 32) : *a, y = *b >= 'A' && *b <= 'Z' ? (char)(*b + 32) : *b;
    if (x != y) return 0;
  }
  return !*a && !*b;
}

static void SampleCount(sample_table_t* table, const char* key) {
  for (int i = 0; i < table->used; ++i)
    if (SameText(table->buckets[i].key, key)) {
      ++table->buckets[i].count;
      return;
    }
  if (table->used == kSampleKeys) {
    ++table->lost;
    return;
  }
  sample_bucket_t* bucket = &table->buckets[table->used++];
  size_t n = 0;
  while (key[n] && n + 1 < kSampleKey) bucket->key[n] = key[n], ++n;
  bucket->key[n] = 0;
  bucket->count = 1;
}

// Names a module's table entry: its file name, whether it is the system's (under C:\windows, where
// Wine's DLLs are, or the shim), and its executable sections.
static void SampleDescribe(sample_module_t* entry, HMODULE module, const WCHAR* path) {
  extern char __ImageBase;
  entry->module = module;
  size_t length = 0, start;
  while (length < 259 && path[length]) ++length;
  start = length;
  while (start && path[start - 1] != '\\' && path[start - 1] != '/') --start;
  size_t n = 0;
  for (size_t i = start; i < length && n + 1 < sizeof(entry->name); ++i)
    entry->name[n++] = path[i] < 0x80 ? (char)path[i] : '?';
  entry->name[n] = 0;
  static const char windows[] = "c:\\windows\\";
  BOOL system = length > 11;
  for (int i = 0; system && windows[i]; ++i) {
    WCHAR c = path[i] >= 'A' && path[i] <= 'Z' ? (WCHAR)(path[i] + 32) : path[i];
    system = c == (WCHAR)windows[i];
  }
  entry->shim = module == (HMODULE)&__ImageBase;
  entry->system = system || entry->shim;
  entry->vulkan = SameText(entry->name, "winevulkan.dll");
  const unsigned char* base = (const unsigned char*)module;
  const unsigned char* nt = base + *(const int32_t*)(base + 0x3c);
  uint16_t sections = *(const uint16_t*)(nt + 6), optional = *(const uint16_t*)(nt + 20);
  const unsigned char* section = nt + 24 + optional;
  entry->code_start = ~0ull;
  entry->code_end = 0;
  for (uint16_t i = 0; i < sections; ++i, section += 40) {
    if (!(*(const uint32_t*)(section + 36) & 0x20000000 /* IMAGE_SCN_MEM_EXECUTE */)) continue;
    uint64_t from = (uint64_t)(uintptr_t)base + *(const uint32_t*)(section + 12);
    uint64_t to = from + *(const uint32_t*)(section + 8);
    if (from < entry->code_start) entry->code_start = from;
    if (to > entry->code_end) entry->code_end = to;
  }
}

// Every loaded module, from a module list; taken while no thread is stopped.
static void SampleModulesRefresh(void) {
  static unsigned char entry[1080];  // MODULEENTRY32W
  HANDLE snapshot = CreateToolhelp32Snapshot(8 /* TH32CS_SNAPMODULE */, 0);
  if (snapshot == INVALID_HANDLE_VALUE) return;
  int count = 0;
  *(DWORD*)entry = sizeof(entry);
  for (BOOL more = Module32FirstW(snapshot, entry); more && count < kSampleModules;
       *(DWORD*)entry = sizeof(entry), more = Module32NextW(snapshot, entry))
    SampleDescribe(&g_sample_modules[count++], *(HMODULE*)(entry + 40), (const WCHAR*)(entry + 560));
  g_sample_module_count = count;
  CloseHandle(snapshot);
}

// The module whose code address is in, from the table, or NULL. A code address outside the table
// (a module loaded since the last list) is looked up with the loader when look_up is set.
static const sample_module_t* SampleModule(uint64_t address, BOOL look_up) {
  for (int i = 0; i < g_sample_module_count; ++i)
    if (address >= g_sample_modules[i].code_start && address < g_sample_modules[i].code_end)
      return &g_sample_modules[i];
  HMODULE module = NULL;
  if (!look_up || address < 0x10000 || g_sample_module_count == kSampleModules ||
      !GetModuleHandleExW(6 /* FROM_ADDRESS | UNCHANGED_REFCOUNT */, (const void*)(uintptr_t)address, &module) ||
      !module)
    return NULL;
  WCHAR path[260];
  DWORD length = GetModuleFileNameW(module, path, 259);
  path[length < 259 ? length : 259] = 0;
  sample_module_t* entry = &g_sample_modules[g_sample_module_count++];
  SampleDescribe(entry, module, path);
  return address >= entry->code_start && address < entry->code_end ? entry : NULL;
}

// The name of the export at or below address in module, when it is within 4 KiB; NULL otherwise.
static const char* NearestExport(HMODULE module, uint64_t address) {
  const unsigned char* base = (const unsigned char*)module;
  const unsigned char* nt = base + *(const int32_t*)(base + 0x3c);
  if (*(const uint16_t*)(nt + 24) != 0x20b) return NULL;  // PE32+
  uint32_t directory = *(const uint32_t*)(nt + 24 + 112), size = *(const uint32_t*)(nt + 24 + 116);
  if (!directory) return NULL;
  const unsigned char* exports = base + directory;
  uint32_t functions_count = *(const uint32_t*)(exports + 20), names_count = *(const uint32_t*)(exports + 24);
  const uint32_t* functions = (const uint32_t*)(base + *(const uint32_t*)(exports + 28));
  const uint32_t* names = (const uint32_t*)(base + *(const uint32_t*)(exports + 32));
  const uint16_t* ordinals = (const uint16_t*)(base + *(const uint32_t*)(exports + 36));
  uint64_t rva = address - (uint64_t)(uintptr_t)base;
  uint32_t best = 0, best_name = 0;
  for (uint32_t i = 0; i < names_count; ++i) {
    if (ordinals[i] >= functions_count) continue;
    uint32_t function = functions[ordinals[i]];
    if (function >= directory && function < directory + size) continue;  // forwarded
    if (function <= rva && function > best) best = function, best_name = names[i];  // NtX before ZwX
  }
  return best_name && rva - best < 4096 ? (const char*)(base + best_name) : NULL;
}

static size_t SampleAppend(char* line, size_t n, size_t capacity, const char* text) {
  while (*text && n + 1 < capacity) line[n++] = *text++;
  line[n] = 0;
  return n;
}

// Names one sample: where it was, and for one in Windows, Wine, Vulkan or outside any module, the
// game module that called.
static void SampleName(uint64_t rip, const uint64_t* words, uint32_t count) {
  char key[kSampleKey];
  key[0] = 0;
  const sample_module_t* module = SampleModule(rip, 1);
  if (!module) {
    // Memory a program allocated and made executable: a JIT's (LuaJIT's); anything else outside the
    // modules is Wine's host side, caught on its way in or out of the host.
    struct { void* base; void* allocation; DWORD allocation_protect; size_t size; DWORD state, protect, type; } region;
    BOOL generated = VirtualQuery((const void*)(uintptr_t)rip, &region, sizeof(region)) &&
                     region.state == 0x1000 /* MEM_COMMIT */ && region.type == 0x20000 /* MEM_PRIVATE */ &&
                     (region.protect & 0xf0) /* PAGE_EXECUTE_* */;
    SampleAppend(key, 0, sizeof(key), generated ? "generated code (JIT)" : "Wine's host side");
  } else if (module->shim) {
    // In the shim's dispatcher, inside a Vulkan call: the caller on the stack is winevulkan's thunk.
    const sample_module_t* caller = NULL;
    uint32_t i = 0;
    for (; i < count && !(caller && caller->vulkan); ++i) caller = SampleModule(words[i], 0);
    const char* call = caller && caller->vulkan ? NearestExport(caller->module, words[i - 1]) : NULL;
    if (caller && caller->vulkan)
      SampleAppend(key, SampleAppend(key, 0, sizeof(key), "Vulkan "), sizeof(key), call ? call : "call");
    else
      SampleAppend(key, 0, sizeof(key), "libcef.dll (the shim)");
  } else {
    size_t n = SampleAppend(key, 0, sizeof(key), module->name);
    const char* name = module->system ? NearestExport(module->module, rip) : NULL;
    if (name) SampleAppend(key, SampleAppend(key, n, sizeof(key), "!"), sizeof(key), name);
  }
  SampleCount(&g_where, key);
  if (module && !module->system) return;
  const char* from = "no game module found";
  for (uint32_t i = 0; i < count; ++i) {
    const sample_module_t* caller = SampleModule(words[i], 0);
    if (caller && !caller->system) {
      from = caller->name;
      break;
    }
  }
  SampleCount(&g_from, from);
}

static size_t SampleNumber(char* line, size_t n, size_t capacity, uint64_t value) {
  char digits[24];
  int d = 0;
  do digits[d++] = (char)('0' + value % 10), value /= 10; while (value && d < 24);
  while (d && n + 1 < capacity) line[n++] = digits[--d];
  line[n] = 0;
  return n;
}

// "key 12.3%, ..." for the most frequent names of table, of total samples.
static size_t SampleTop(char* line, size_t n, size_t capacity, sample_table_t* table, long total, int top) {
  uint64_t listed[(kSampleKeys + 63) / 64] = {0, 0};
  for (int k = 0; k < top; ++k) {
    int best = -1;
    for (int i = 0; i < table->used; ++i)
      if (!(listed[i / 64] >> (i % 64) & 1) && (best < 0 || table->buckets[i].count > table->buckets[best].count))
        best = i;
    if (best < 0) break;
    listed[best / 64] |= 1ull << (best % 64);
    uint64_t tenths = (uint64_t)table->buckets[best].count * 1000 / (uint64_t)total;
    if (k) n = SampleAppend(line, n, capacity, ", ");
    n = SampleAppend(line, n, capacity, table->buckets[best].key);
    n = SampleAppend(line, n, capacity, " ");
    n = SampleNumber(line, n, capacity, tenths / 10);
    n = SampleAppend(line, n, capacity, ".");
    n = SampleNumber(line, n, capacity, tenths % 10);
    n = SampleAppend(line, n, capacity, "%");
  }
  return n;
}

static void SampleReport(long samples, long failed, int64_t held, int64_t longest, int64_t frequency) {
  static char line[1600];
  if (!samples) {
    LogNumber("libcef-shim: sample: none taken, failed ", (uintptr_t)failed);
    return;
  }
  size_t n = SampleAppend(line, 0, sizeof(line), "libcef-shim: sample: ");
  n = SampleNumber(line, n, sizeof(line), (uint64_t)samples);
  n = SampleAppend(line, n, sizeof(line), " of the thread that submits, each held it ");
  uint64_t hundredths = (uint64_t)held * 100000 / (uint64_t)frequency / (uint64_t)samples;
  n = SampleNumber(line, n, sizeof(line), hundredths / 100);
  n = SampleAppend(line, n, sizeof(line), ".");
  n = SampleNumber(line, n, sizeof(line), hundredths / 10 % 10);
  n = SampleNumber(line, n, sizeof(line), hundredths % 10);
  n = SampleAppend(line, n, sizeof(line), " ms, longest ");
  n = SampleNumber(line, n, sizeof(line), (uint64_t)longest * 1000 / (uint64_t)frequency);
  n = SampleAppend(line, n, sizeof(line), " ms");
  if (failed) {
    n = SampleAppend(line, n, sizeof(line), ", ");
    n = SampleNumber(line, n, sizeof(line), (uint64_t)failed);
    n = SampleAppend(line, n, sizeof(line), " failed");
  }
  n = SampleAppend(line, n, sizeof(line), "; where it was: ");
  n = SampleTop(line, n, sizeof(line), &g_where, samples, kSampleTop);
  SampleAppend(line, n, sizeof(line), "\n");
  Log(line);
  if (g_from.used) {
    n = SampleAppend(line, 0, sizeof(line),
                     "libcef-shim: sample: in Windows, Wine, Vulkan or outside any module, called from: ");
    n = SampleTop(line, n, sizeof(line), &g_from, samples, 8);
    SampleAppend(line, n, sizeof(line), "\n");
    Log(line);
  }
  g_where.used = g_from.used = 0;
  g_where.lost = g_from.lost = 0;
}

static DWORD WINAPI SampleLater(void* unused) {
  (void)unused;
  typedef long(WINAPI * query_thread_fn)(HANDLE, int, void*, DWORD, DWORD*);
  static const WCHAR ntdll_name[] = {'n', 't', 'd', 'l', 'l', '.', 'd', 'l', 'l', 0};
  HMODULE ntdll = GetModuleHandleW(ntdll_name);
  query_thread_fn query = ntdll ? (query_thread_fn)GetProcAddress(ntdll, "NtQueryInformationThread") : NULL;
  static _Alignas(16) unsigned char context[1232];
  static uint64_t words[kSampleWords];
  int64_t frequency = 0, cycle = 0, held = 0, longest = 0;
  long samples = 0, failed = 0;
  DWORD id = 0;
  HANDLE thread = NULL;
  const volatile uint64_t* teb = NULL;
  QueryPerformanceFrequency(&frequency);
  if (!query || !frequency) {
    Log("libcef-shim: sample: off, NtQueryInformationThread or the counter is missing\n");
    return 0;
  }
  for (;;) {
    Sleep(kSamplePeriodMs);
    DWORD current = g_vkt.thread;
    if (!current) continue;
    if (current != id) {
      if (thread) CloseHandle(thread);
      id = current;
      teb = NULL;
      thread = OpenThread(0x004a /* SUSPEND_RESUME | GET_CONTEXT | QUERY_INFORMATION */, 0, id);
      struct {
        long exit_status;
        void* teb;
        uint64_t client_id[2];
        uint64_t affinity;
        long priority, base_priority;
      } info;
      if (thread && query(thread, 0 /* ThreadBasicInformation */, &info, sizeof(info), NULL) == 0 && info.teb)
        teb = (const volatile uint64_t*)info.teb;
      LogNumber(teb ? "libcef-shim: sample: the thread that submits is "
                    : "libcef-shim: sample: cannot sample thread ",
                id);
      SampleModulesRefresh();
      cycle = 0;
    }
    if (!teb) continue;
    int64_t now = 0, start = 0, end = 0;
    QueryPerformanceCounter(&now);
    if (!cycle || now - cycle >= kSampleCycleSeconds * frequency) {
      cycle = now;
      Log("libcef-shim: sample: sampling for 30 s\n");
    }
    if (now - cycle >= kSampleOnSeconds * frequency) {
      if (samples || failed) {
        SampleReport(samples, failed, held, longest, frequency);
        SampleModulesRefresh();
        samples = failed = 0;
        held = longest = 0;
      }
      continue;
    }
    uint64_t rip = 0;
    uint32_t count = 0;
    QueryPerformanceCounter(&start);
    if (SuspendThread(thread) == (DWORD)-1) {
      ++failed;
      continue;
    }
    *(DWORD*)(context + 0x30) = 0x100001;  // CONTEXT_CONTROL
    *(uint64_t*)(context + CONTEXT_RIP) = 0;
    *(uint64_t*)(context + CONTEXT_RSP) = 0;
    if (GetThreadContext(thread, context)) {
      rip = *(uint64_t*)(context + CONTEXT_RIP);
      uint64_t rsp = *(uint64_t*)(context + CONTEXT_RSP);
      uint64_t stack_base = teb[1], stack_limit = teb[2];  // NT_TIB
      if (rsp >= stack_limit && rsp < stack_base) {
        count = (uint32_t)((stack_base - rsp) / 8);
        if (count > kSampleWords) count = kSampleWords;
        for (uint32_t i = 0; i < count; ++i) words[i] = ((const volatile uint64_t*)(uintptr_t)rsp)[i];
      }
    }
    ResumeThread(thread);
    QueryPerformanceCounter(&end);
    if (!rip) {
      ++failed;
      continue;
    }
    held += end - start;
    if (end - start > longest) longest = end - start;
    ++samples;
    SampleName(rip, words, count);
  }
  return 0;
}

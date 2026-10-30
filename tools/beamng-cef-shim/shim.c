// SPDX-License-Identifier: GPL-3.0-or-later
//
// libcef.dll stand-in for games that embed CEF (BeamNG.drive). Every export is
// forwarded to the real library, renamed libcef_real.dll, except cef_initialize:
// it hooks the game's CefApp::OnBeforeCommandLineProcessing to append switches
// that keep Chromium's GPU, network, storage and renderer work inside the game's
// own process. A PS5 title cannot start processes, so CEF's helpers never run.
//
// The switches come from libcef_shim.txt next to this DLL (one per line,
// "name" or "name=value", '#' starts a comment). Without that file the defaults
// below are used. Each step is logged to stderr, which lands in the title log.
//
// Freestanding: no C runtime, only kernel32.

#include <stddef.h>
#include <stdint.h>

typedef void* HANDLE;
typedef void* HMODULE;
typedef int BOOL;
typedef unsigned long DWORD;
typedef uint16_t WCHAR;

#define WINAPI __stdcall
#define DLL_PROCESS_ATTACH 1
#define STD_ERROR_HANDLE ((DWORD)-12)
#define GENERIC_READ 0x80000000UL
#define FILE_SHARE_READ 1
#define OPEN_EXISTING 3
#define INVALID_HANDLE_VALUE ((HANDLE)(intptr_t)-1)
#define PAGE_READWRITE 0x04

__declspec(dllimport) HMODULE WINAPI LoadLibraryW(const WCHAR*);
__declspec(dllimport) void* WINAPI GetProcAddress(HMODULE, const char*);
__declspec(dllimport) DWORD WINAPI GetModuleFileNameW(HMODULE, WCHAR*, DWORD);
__declspec(dllimport) HANDLE WINAPI GetStdHandle(DWORD);
__declspec(dllimport) BOOL WINAPI WriteFile(HANDLE, const void*, DWORD, DWORD*, void*);
__declspec(dllimport) BOOL WINAPI ReadFile(HANDLE, void*, DWORD, DWORD*, void*);
__declspec(dllimport) HANDLE WINAPI CreateFileW(const WCHAR*, DWORD, DWORD, void*, DWORD, DWORD, HANDLE);
__declspec(dllimport) BOOL WINAPI CloseHandle(HANDLE);
__declspec(dllimport) BOOL WINAPI VirtualProtect(void*, size_t, DWORD, DWORD*);
__declspec(dllimport) size_t WINAPI VirtualQuery(const void*, void*, size_t);
__declspec(dllimport) void* WINAPI AddVectoredExceptionHandler(DWORD, void*);
__declspec(dllimport) BOOL WINAPI GetModuleHandleExW(DWORD, const void*, HMODULE*);
__declspec(dllimport) DWORD WINAPI GetCurrentThreadId(void);
__declspec(dllimport) DWORD WINAPI GetCurrentProcessId(void);
__declspec(dllimport) HANDLE WINAPI CreateThread(void*, size_t, void*, void*, DWORD, DWORD*);
__declspec(dllimport) void WINAPI Sleep(DWORD);
__declspec(dllimport) HANDLE WINAPI CreateToolhelp32Snapshot(DWORD, DWORD);
__declspec(dllimport) BOOL WINAPI Thread32First(HANDLE, void*);
__declspec(dllimport) BOOL WINAPI Thread32Next(HANDLE, void*);
__declspec(dllimport) HANDLE WINAPI OpenThread(DWORD, BOOL, DWORD);
__declspec(dllimport) DWORD WINAPI SuspendThread(HANDLE);
__declspec(dllimport) DWORD WINAPI ResumeThread(HANDLE);
__declspec(dllimport) BOOL WINAPI GetThreadContext(HANDLE, void*);
__declspec(dllimport) HMODULE WINAPI GetModuleHandleW(const WCHAR*);
__declspec(dllimport) uint64_t WINAPI GetTickCount64(void);
__declspec(dllimport) BOOL WINAPI FlushInstructionCache(HANDLE, const void*, size_t);
__declspec(dllimport) BOOL WINAPI Module32FirstW(HANDLE, void*);
__declspec(dllimport) BOOL WINAPI Module32NextW(HANDLE, void*);
__declspec(dllimport) void* WINAPI VirtualAlloc(void*, size_t, DWORD, DWORD);
__declspec(dllimport) void* WINAPI RtlLookupFunctionEntry(uint64_t, uint64_t*, void*);
__declspec(dllimport) void* WINAPI RtlVirtualUnwind(DWORD, uint64_t, uint64_t, void*, void*, void**, uint64_t*, void*);

// CEF C API, Windows: cef_string_t is UTF-16. Only the parts used here.
typedef struct {
  WCHAR* str;
  size_t length;
  void (*dtor)(WCHAR*);
} cef_string_t;

typedef struct cef_base_ref_counted_t {
  size_t size;
  void* add_ref;
  void* release;
  void* has_one_ref;
  void* has_at_least_one_ref;
} cef_base_ref_counted_t;

// Method order of CefCommandLine, unchanged across CEF versions up to append_switch_with_value.
typedef struct cef_command_line_t {
  cef_base_ref_counted_t base;
  int (*is_valid)(struct cef_command_line_t*);
  int (*is_read_only)(struct cef_command_line_t*);
  void* copy;
  void* init_from_argv;
  void* init_from_string;
  void* reset;
  void* get_argv;
  cef_string_t* (*get_command_line_string)(struct cef_command_line_t*);
  void* get_program;
  void* set_program;
  void* has_switches;
  int (*has_switch)(struct cef_command_line_t*, const cef_string_t*);
  void* get_switch_value;
  void* get_switches;
  void (*append_switch)(struct cef_command_line_t*, const cef_string_t*);
  void (*append_switch_with_value)(struct cef_command_line_t*, const cef_string_t*, const cef_string_t*);
  void (*remove_switch)(struct cef_command_line_t*, const cef_string_t*);  // CEF 141 and later
} cef_command_line_t;

typedef struct cef_app_t {
  cef_base_ref_counted_t base;
  void (*on_before_command_line_processing)(struct cef_app_t*, const cef_string_t* process_type,
                                            cef_command_line_t* command_line);
} cef_app_t;

typedef int (*cef_initialize_fn)(const void* args, const void* settings, cef_app_t* app, void* sandbox_info);
typedef void (*cef_string_userfree_free_fn)(cef_string_t*);

static const char* const kDefaultSwitches[] = {"single-process", "in-process-gpu", "no-sandbox",
                                               "disable-stack-profiler", NULL};

static HMODULE g_real;
static void (*g_original_hook)(cef_app_t*, const cef_string_t*, cef_command_line_t*);
static BOOL g_no_hook;
static BOOL g_keep_settings;
static BOOL g_keep_profiler;
static BOOL g_no_commit;
static BOOL g_vk_fix;
static BOOL g_vk_reuse;
static BOOL g_no_shared_texture;
static BOOL g_keep_begin_frame;
static BOOL g_no_overlay;
static char g_config[4096];
static DWORD g_config_size;
static BOOL g_have_config;

// The compiler may call these for struct copies; there is no C runtime.
void* memcpy(void* to, const void* from, size_t count) {
  unsigned char* d = to;
  const unsigned char* f = from;
  while (count--) *d++ = *f++;
  return to;
}
void* memset(void* to, int value, size_t count) {
  unsigned char* d = to;
  while (count--) *d++ = (unsigned char)value;
  return to;
}

static size_t Length(const char* s) {
  size_t n = 0;
  while (s[n]) ++n;
  return n;
}

// Also written to libcef_shim.log beside this DLL: Playden keeps only the start and end of a
// long title log, and the shim's lines can fall in the part it leaves out.
static HANDLE g_log_file = INVALID_HANDLE_VALUE;

static void Log(const char* text) {
  DWORD written;
  HANDLE err = GetStdHandle(STD_ERROR_HANDLE);
  if (err && err != INVALID_HANDLE_VALUE) WriteFile(err, text, (DWORD)Length(text), &written, NULL);
  if (g_log_file != INVALID_HANDLE_VALUE) WriteFile(g_log_file, text, (DWORD)Length(text), &written, NULL);
}

static void LogWide(const char* prefix, const WCHAR* text, size_t length) {
  char line[1024];
  size_t n = 0, p = Length(prefix);
  for (size_t i = 0; i < p && n < sizeof(line) - 2; ++i) line[n++] = prefix[i];
  for (size_t i = 0; i < length && n < sizeof(line) - 2; ++i) line[n++] = text[i] < 0x80 ? (char)text[i] : '?';
  line[n++] = '\n';
  line[n] = 0;
  Log(line);
}

static void LogNumber(const char* prefix, uintptr_t value) {
  char line[96], digits[20];
  size_t n = 0, d = 0, p = Length(prefix);
  for (size_t i = 0; i < p && n < 64; ++i) line[n++] = prefix[i];
  do digits[d++] = "0123456789abcdef"[value & 15], value >>= 4; while (value && d < sizeof(digits));
  line[n++] = '0';
  line[n++] = 'x';
  while (d) line[n++] = digits[--d];
  line[n++] = '\n';
  line[n] = 0;
  Log(line);
}

// "prefix module+0xoffset" for a code address, or "prefix 0xaddress" outside any module.
static void LogAddress(const char* prefix, uint64_t address) {
  HMODULE module = NULL;
  WCHAR path[260];
  if (address && GetModuleHandleExW(6 /* FROM_ADDRESS | UNCHANGED_REFCOUNT */, (const void*)address, &module) &&
      module) {
    DWORD n = GetModuleFileNameW(module, path, 260);
    DWORD start = n;
    while (start && path[start - 1] != '\\' && path[start - 1] != '/') --start;
    char line[160];
    size_t k = 0, p = Length(prefix);
    for (size_t i = 0; i < p && k < 60; ++i) line[k++] = prefix[i];
    for (DWORD i = start; i < n && k < 120; ++i) line[k++] = path[i] < 0x80 ? (char)path[i] : '?';
    line[k++] = '+';
    line[k] = 0;
    LogNumber(line, address - (uint64_t)(uintptr_t)module);
  } else {
    LogNumber(prefix, address);
  }
}

// x64 CONTEXT and EXCEPTION_RECORD, by offset: no Windows headers here.
#define CONTEXT_RSP 0x98
#define CONTEXT_RIP 0xf8
typedef struct {
  DWORD code;
  DWORD flags;
  void* next;
  void* address;
  DWORD parameter_count;
  uint64_t parameters[15];
} exception_record_t;
typedef struct {
  exception_record_t* record;
  unsigned char* context;
} exception_pointers_t;

static long g_reported;

static BOOL Interesting(DWORD code) {
  return code == 0xc0000409 || code == 0xc0000005 || code == 0xc000001d || code == 0x80000003 ||
         code == 0xc0000374 || code == 0xc0000096 || code == 0xc00000fd || code == 0xe06d7363;
}

// Logs crashes and C++ exceptions with a call stack, then lets them continue as before.
static long g_committed;

static long WINAPI ExceptionLogger(exception_pointers_t* info) {
  exception_record_t* record = info->record;
  // A read of a page that is reserved but not committed, such as a stack page below what its
  // thread has used (Chromium reading another thread's stack hit one at 0x1000114000): commit
  // that host page and let the read run again, rather than end the game.
  if (record->code == 0xc0000005 && record->parameter_count >= 2 && record->parameters[0] == 0 &&
      g_committed < 64 && !g_no_commit) {
    struct { void* base; void* allocation; DWORD allocation_protect; size_t size; DWORD state, protect, type; } region;
    void* page = (void*)(uintptr_t)(record->parameters[1] & ~(uint64_t)0x3fff);
    BOOL known = VirtualQuery(page, &region, sizeof(region)) != 0;
    if (known && region.state == 0x2000 /* MEM_RESERVE */ && region.type == 0x20000 /* MEM_PRIVATE */ &&
        VirtualAlloc(page, 0x4000, 0x1000 /* MEM_COMMIT */, PAGE_READWRITE)) {
      ++g_committed;
      LogAddress("libcef-shim: committed a reserved page read by ", (uint64_t)(uintptr_t)record->address);
      LogNumber("libcef-shim:   page ", (uint64_t)(uintptr_t)page);
      return -1;  // EXCEPTION_CONTINUE_EXECUTION
    }
    if (known) {
      LogNumber("libcef-shim: read fault page ", (uint64_t)(uintptr_t)page);
      LogNumber("libcef-shim:   state ", region.state);
      LogNumber("libcef-shim:   protect ", region.protect);
      LogNumber("libcef-shim:   type ", region.type);
      LogNumber("libcef-shim:   region base ", (uint64_t)(uintptr_t)region.base);
      LogNumber("libcef-shim:   region size ", region.size);
      LogNumber("libcef-shim:   allocation base ", (uint64_t)(uintptr_t)region.allocation);
    }
    // Chromium reading another thread's stack past what is committed, into a no-access page
    // (a stack guard) or a free one: let the read see zeros instead of ending the game.
    HMODULE reader = NULL;
    static const WCHAR real_name[] = {'l', 'i', 'b', 'c', 'e', 'f', '_', 'r', 'e', 'a', 'l', '.', 'd', 'l', 'l', 0};
    BOOL from_cef = GetModuleHandleExW(6, record->address, &reader) && reader && reader == GetModuleHandleW(real_name);
    if (known && from_cef && region.state == 0x1000 /* MEM_COMMIT */ && region.protect == 0x01 /* NOACCESS */) {
      DWORD old_protect;
      if (VirtualProtect(page, 0x4000, PAGE_READWRITE, &old_protect)) {
        ++g_committed;
        LogAddress("libcef-shim: opened a no-access page read by ", (uint64_t)(uintptr_t)record->address);
        return -1;
      }
    }
    if (known && from_cef && region.state == 0x10000 /* MEM_FREE */ &&
        VirtualAlloc(page, 0x4000, 0x3000 /* MEM_RESERVE | MEM_COMMIT */, PAGE_READWRITE)) {
      ++g_committed;
      LogAddress("libcef-shim: mapped a zero page for a read of free memory by ", (uint64_t)(uintptr_t)record->address);
      return -1;
    }
  }
  if (!Interesting(record->code) || g_reported >= 12) return 0;
  ++g_reported;
  LogNumber("libcef-shim: exception code ", record->code);
  LogNumber("libcef-shim:   thread ", GetCurrentThreadId());
  LogNumber("libcef-shim:   flags ", record->flags);
  LogAddress("libcef-shim:   at ", (uint64_t)(uintptr_t)record->address);
  for (DWORD i = 0; i < record->parameter_count && i < 4; ++i) LogNumber("libcef-shim:   parameter ", record->parameters[i]);
  static _Alignas(16) unsigned char context[1232];
  for (size_t i = 0; i < sizeof(context); ++i) context[i] = info->context[i];
  for (int frame = 0; frame < 32; ++frame) {
    uint64_t* rip = (uint64_t*)(context + CONTEXT_RIP);
    uint64_t* rsp = (uint64_t*)(context + CONTEXT_RSP);
    if (!*rip) break;
    LogAddress("libcef-shim:   frame ", *rip);
    uint64_t base = 0, establisher = 0;
    void* handler_data = NULL;
    void* entry = RtlLookupFunctionEntry(*rip, &base, NULL);
    if (entry) {
      RtlVirtualUnwind(0, base, *rip, entry, context, &handler_data, &establisher, NULL);
    } else {
      *rip = *(uint64_t*)(uintptr_t)*rsp;
      *rsp += 8;
    }
  }
  return 0;  // EXCEPTION_CONTINUE_SEARCH
}

// Appends "module+0xoffset" (or "0xaddress") for address to line.
static size_t FormatAddress(char* line, size_t n, size_t capacity, uint64_t address) {
  HMODULE module = NULL;
  WCHAR path[260];
  uint64_t offset = address;
  if (GetModuleHandleExW(6, (const void*)address, &module) && module) {
    DWORD length = GetModuleFileNameW(module, path, 260), start = length;
    while (start && path[start - 1] != '\\' && path[start - 1] != '/') --start;
    for (DWORD i = start; i < length && n + 1 < capacity; ++i) line[n++] = path[i] < 0x80 ? (char)path[i] : '?';
    if (n + 1 < capacity) line[n++] = '+';
    offset = address - (uint64_t)(uintptr_t)module;
  }
  char digits[20];
  int d = 0;
  do digits[d++] = "0123456789abcdef"[offset & 15], offset >>= 4; while (offset && d < 20);
  if (n + 3 < capacity) line[n++] = '0', line[n++] = 'x';
  while (d && n + 1 < capacity) line[n++] = digits[--d];
  return n;
}

static BOOL InModule(uint64_t address) {
  HMODULE module = NULL;
  return address > 0x10000 && GetModuleHandleExW(6, (const void*)address, &module) && module;
}

// One line per thread: where it is now, then the code addresses found on its stack (a scan, so
// some are stale). Each thread is suspended only to copy its registers and stack; nothing that
// takes a lock runs while it is suspended.
static void DumpThreads(void) {
  typedef struct { DWORD size, usage, thread_id, process_id; long base_priority, delta_priority; DWORD flags; } thread_entry_t;
  static _Alignas(16) unsigned char context[1232];
  static uint64_t stack[1024];
  DWORD self = GetCurrentThreadId(), process = GetCurrentProcessId();
  HANDLE snapshot = CreateToolhelp32Snapshot(4 /* TH32CS_SNAPTHREAD */, 0);
  if (snapshot == INVALID_HANDLE_VALUE) {
    Log("libcef-shim: thread dump: no snapshot\n");
    return;
  }
  Log("libcef-shim: thread dump begins\n");
  thread_entry_t entry;
  entry.size = sizeof(entry);
  for (BOOL more = Thread32First(snapshot, &entry); more; entry.size = sizeof(entry), more = Thread32Next(snapshot, &entry)) {
    if (entry.process_id != process || entry.thread_id == self) continue;
    HANDLE thread = OpenThread(0x004a /* SUSPEND_RESUME | GET_CONTEXT | QUERY_INFORMATION */, 0, entry.thread_id);
    if (!thread) continue;
    size_t words = 0;
    uint64_t rip = 0, rsp = 0;
    if (SuspendThread(thread) != (DWORD)-1) {
      for (size_t i = 0; i < sizeof(context); ++i) context[i] = 0;
      *(DWORD*)(context + 0x30) = 0x10000b;  // CONTEXT_FULL
      if (GetThreadContext(thread, context)) {
        rip = *(uint64_t*)(context + CONTEXT_RIP);
        rsp = *(uint64_t*)(context + CONTEXT_RSP);
        // Copy up to 8 KiB of stack, stopping at the end of its committed region.
        struct { void* base; void* allocation; DWORD allocation_protect; size_t size; DWORD state, protect, type; } region;
        size_t limit = 1024;
        if (VirtualQuery((void*)rsp, &region, sizeof(region))) {
          size_t left = ((uintptr_t)region.base + region.size - rsp) / 8;
          if (left < limit) limit = left;
        }
        for (words = 0; words < limit; ++words) stack[words] = ((uint64_t*)(uintptr_t)rsp)[words];
      }
      ResumeThread(thread);
    }
    CloseHandle(thread);
    char line[1200];
    size_t n = 0;
    const char* head = "libcef-shim: thread ";
    while (*head) line[n++] = *head++;
    n = FormatAddress(line, n, sizeof(line), entry.thread_id);
    line[n++] = ' ';
    line[n++] = 'a';
    line[n++] = 't';
    line[n++] = ' ';
    n = FormatAddress(line, n, sizeof(line), rip);
    int found = 0;
    for (size_t i = 0; i < words && found < 14 && n + 80 < sizeof(line); ++i) {
      if (!InModule(stack[i])) continue;
      line[n++] = ' ';
      line[n++] = '<';
      n = FormatAddress(line, n, sizeof(line), stack[i]);
      ++found;
    }
    line[n++] = '\n';
    line[n] = 0;
    Log(line);
  }
  CloseHandle(snapshot);
  Log("libcef-shim: thread dump ends\n");
}

static DWORD g_dump_seconds = 40;

static DWORD WINAPI DumpLater(void* unused) {
  (void)unused;
  for (int round = 0; round < 2; ++round) {
    Sleep(g_dump_seconds * 1000);
    DumpThreads();
  }
  return 0;
}

// Directory of this DLL, with a trailing backslash, into dir. Returns its length.
static DWORD ModuleDirectory(WCHAR* dir, DWORD capacity) {
  extern char __ImageBase;
  DWORD n = GetModuleFileNameW((HMODULE)&__ImageBase, dir, capacity);
  while (n && dir[n - 1] != '\\' && dir[n - 1] != '/') --n;
  return n;
}

static void LoadConfig(void) {
  WCHAR path[1024];
  static const char name[] = "libcef_shim.txt";
  DWORD n = ModuleDirectory(path, 1000);
  for (size_t i = 0; name[i]; ++i) path[n++] = (WCHAR)name[i];
  path[n] = 0;
  HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
  if (file == INVALID_HANDLE_VALUE) {
    Log("libcef-shim: no libcef_shim.txt, using the default switches\n");
    return;
  }
  if (ReadFile(file, g_config, sizeof(g_config) - 1, &g_config_size, NULL)) {
    g_config[g_config_size] = 0;
    g_have_config = 1;
    Log("libcef-shim: switches from libcef_shim.txt\n");
    for (const char* p = g_config; *p; ++p) {
      static const char option[] = "shim:nohook";
      size_t i = 0;
      while (option[i] && p[i] == option[i]) ++i;
      if (!option[i]) g_no_hook = 1;
      static const char dump_off[] = "shim:nodump";
      i = 0;
      while (dump_off[i] && p[i] == dump_off[i]) ++i;
      if (!dump_off[i]) g_dump_seconds = 0;
      static const char dump_after[] = "shim:dump-after=";
      i = 0;
      while (dump_after[i] && p[i] == dump_after[i]) ++i;
      if (!dump_after[i]) {
        DWORD seconds = 0;
        for (const char* d = p + i; *d >= '0' && *d <= '9'; ++d) seconds = seconds * 10 + (DWORD)(*d - '0');
        g_dump_seconds = seconds;
      }
      static const char no_fix[] = "shim:no-vk-fix";
      i = 0;
      while (no_fix[i] && p[i] == no_fix[i]) ++i;
      if (!no_fix[i]) g_vk_fix = 0;
      static const char no_overlay[] = "shim:no-overlay";
      i = 0;
      while (no_overlay[i] && p[i] == no_overlay[i]) ++i;
      if (!no_overlay[i]) g_no_overlay = 1;
      static const char keep_begin[] = "shim:keep-begin-frame";
      i = 0;
      while (keep_begin[i] && p[i] == keep_begin[i]) ++i;
      if (!keep_begin[i]) g_keep_begin_frame = 1;
      static const char no_shared[] = "shim:no-shared-texture";
      i = 0;
      while (no_shared[i] && p[i] == no_shared[i]) ++i;
      if (!no_shared[i]) g_no_shared_texture = 1;
      static const char no_reuse[] = "shim:no-vk-reuse";
      i = 0;
      while (no_reuse[i] && p[i] == no_reuse[i]) ++i;
      if (!no_reuse[i]) g_vk_reuse = 0;
      static const char no_commit[] = "shim:no-commit";
      i = 0;
      while (no_commit[i] && p[i] == no_commit[i]) ++i;
      if (!no_commit[i]) g_no_commit = 1;
      static const char keep_profiler[] = "shim:keep-profiler";
      i = 0;
      while (keep_profiler[i] && p[i] == keep_profiler[i]) ++i;
      if (!keep_profiler[i]) g_keep_profiler = 1;
      static const char keep[] = "shim:keep-settings";
      i = 0;
      while (keep[i] && p[i] == keep[i]) ++i;
      if (!keep[i]) g_keep_settings = 1;
    }
  }
  CloseHandle(file);
}

// This CEF's string type. The C API passes cef_string_t, whose characters are UTF-16 in CEF's
// default build and UTF-8 in BeamNG's (seen on the console: its command line came back as
// bytes). Same layout either way: characters, their count, a destructor.
static BOOL g_cef_utf8;

// A CEF string of ASCII text, in storage (at least 2 * length bytes).
static cef_string_t MakeCefString(const char* ascii, size_t length, void* storage) {
  if (g_cef_utf8) {
    char* bytes = storage;
    for (size_t i = 0; i < length; ++i) bytes[i] = ascii[i];
  } else {
    WCHAR* wide = storage;
    for (size_t i = 0; i < length; ++i) wide[i] = (unsigned char)ascii[i];
  }
  return (cef_string_t){storage, length, NULL};
}

static void LogCefString(const char* prefix, const cef_string_t* text) {
  if (!text || !text->str) return;
  if (!g_cef_utf8) {
    LogWide(prefix, text->str, text->length);
    return;
  }
  char line[1024];
  size_t n = 0, p = Length(prefix);
  const unsigned char* bytes = (const unsigned char*)text->str;
  for (size_t i = 0; i < p && n < sizeof(line) - 2; ++i) line[n++] = prefix[i];
  for (size_t i = 0; i < text->length && n < sizeof(line) - 2; ++i) line[n++] = bytes[i] < 0x80 && bytes[i] ? bytes[i] : '?';
  line[n++] = '\n';
  line[n] = 0;
  Log(line);
}

// Reads the string type from a string CEF made: an ASCII character in UTF-16 has a zero high byte.
static void DetectCefStringType(cef_command_line_t* command_line) {
  cef_string_t* line = command_line->get_command_line_string(command_line);
  if (!line || !line->str || line->length < 2) return;
  const unsigned char* bytes = (const unsigned char*)line->str;
  g_cef_utf8 = bytes[0] < 0x80 && bytes[1] != 0;
  Log(g_cef_utf8 ? "libcef-shim: this CEF's strings are UTF-8\n" : "libcef-shim: this CEF's strings are UTF-16\n");
  typedef void (*free_fn)(cef_string_t*);
  free_fn free_string = (free_fn)GetProcAddress(
      g_real, g_cef_utf8 ? "cef_string_userfree_utf8_free" : "cef_string_userfree_utf16_free");
  if (free_string) free_string(line);
}

static void Append(cef_command_line_t* command_line, const char* name, size_t name_length, const char* value,
                   size_t value_length) {
  WCHAR name_storage[256], value_storage[512];
  if (name_length >= 256 || value_length >= 512) return;
  cef_string_t cef_name = MakeCefString(name, name_length, name_storage);
  cef_string_t cef_value = MakeCefString(value ? value : "", value_length, value_storage);
  if (command_line->has_switch(command_line, &cef_name)) {
    LogCefString("libcef-shim: already set: ", &cef_name);
    if (!value) return;
  }
  if (value)
    command_line->append_switch_with_value(command_line, &cef_name, &cef_value);
  else
    command_line->append_switch(command_line, &cef_name);
  LogCefString("libcef-shim: appended ", &cef_name);
}

static BOOL IsSpace(char c) { return c == ' ' || c == '\t' || c == '\r'; }

static void AppendConfigured(cef_command_line_t* command_line) {
  if (!g_have_config) {
    for (int i = 0; kDefaultSwitches[i]; ++i)
      Append(command_line, kDefaultSwitches[i], Length(kDefaultSwitches[i]), NULL, 0);
    return;
  }
  const char* p = g_config;
  const char* end = g_config + g_config_size;
  while (p < end) {
    const char* line_end = p;
    while (line_end < end && *line_end != '\n') ++line_end;
    const char* s = p;
    const char* e = line_end;
    while (s < e && IsSpace(*s)) ++s;
    if (e - s >= 11 && s[0] == 's' && s[1] == 'h' && s[2] == 'i' && s[3] == 'm' && s[4] == ':') {
      p = line_end + 1;  // shim:... options were read by LoadConfig
      continue;
    }
    while (s < e && (*s == '-')) ++s;  // "--single-process" and "single-process" both work
    for (const char* h = s; h < e; ++h)
      if (*h == '#') e = h;
    while (e > s && IsSpace(e[-1])) --e;
    if (e > s) {
      const char* equals = s;
      while (equals < e && *equals != '=') ++equals;
      if (equals < e)
        Append(command_line, s, (size_t)(equals - s), equals + 1, (size_t)(e - equals - 1));
      else
        Append(command_line, s, (size_t)(e - s), NULL, 0);
    }
    p = line_end + 1;
  }
}

static void AppendAndLog(cef_command_line_t* command_line);

// "prefix" and the first code units of a CEF string in hex, to see what the string really holds.
static void LogHex(const char* prefix, const cef_string_t* text) {
  char line[400];
  size_t n = 0, p = Length(prefix);
  for (size_t i = 0; i < p && n < 80; ++i) line[n++] = prefix[i];
  size_t count = text && text->str ? (text->length < 40 ? text->length : 40) : 0;
  for (size_t i = 0; i < count; ++i) {
    WCHAR c = text->str[i];
    line[n++] = ' ';
    for (int shift = 12; shift >= 0; shift -= 4) line[n++] = "0123456789abcdef"[(c >> shift) & 15];
  }
  line[n++] = '\n';
  line[n] = 0;
  Log(line);
  LogNumber("libcef-shim:   string length ", text ? text->length : 0);
}

// Logs whether each wanted switch is on command_line.
static void CheckSwitches(const char* where, cef_command_line_t* command_line) {
  static const char* const names[] = {"single-process", "in-process-gpu", "no-sandbox", NULL};
  for (int k = 0; names[k]; ++k) {
    WCHAR storage[32];
    cef_string_t name = MakeCefString(names[k], Length(names[k]), storage);
    char line[160];
    size_t n = 0;
    const char* parts[] = {"libcef-shim: ", where, ": ", names[k],
                           command_line->has_switch(command_line, &name) ? " present\n" : " MISSING\n", NULL};
    for (int j = 0; parts[j]; ++j)
      for (const char* c = parts[j]; *c && n < sizeof(line) - 1; ++c) line[n++] = *c;
    line[n] = 0;
    Log(line);
  }
}

static void HookedOnBeforeCommandLineProcessing(cef_app_t* self, const cef_string_t* process_type,
                                                cef_command_line_t* command_line) {
  Log("libcef-shim: CEF asked for the command line; calling BeamNG's handler first\n");
  // A struct argument carries one reference that the callee releases (BeamNG's handler does),
  // so keep a reference of our own to use the command line after it returns.
  typedef void (*add_ref_fn)(void*);
  typedef int (*release_fn)(void*);
  if (command_line) ((add_ref_fn)command_line->base.add_ref)(command_line);
  if (g_original_hook) {
    g_original_hook(self, process_type, command_line);
  } else if (command_line) {
    ((release_fn)command_line->base.release)(command_line);  // what a handler would have done
  }
  Log("libcef-shim: BeamNG's handler returned\n");
  if (!command_line) {
    Log("libcef-shim: no command line, nothing appended\n");
    return;
  }
  if (process_type && process_type->length) {
    LogCefString("libcef-shim: leaving a helper process alone: ", process_type);
  } else if (command_line->is_read_only(command_line)) {
    Log("libcef-shim: command line is read-only, nothing appended\n");
  } else {
    DetectCefStringType(command_line);
    AppendAndLog(command_line);
  }
  ((release_fn)command_line->base.release)(command_line);
}

// Chromium's stack sampling profiler, which BeamNG turns on, reads other threads' stacks; on the
// PS5 a stack page not yet committed faults (read access at 0x1000114000) and ends the game.
static void RemoveStackProfiler(cef_command_line_t* command_line) {
  WCHAR storage[32];
  cef_string_t name = MakeCefString("start-stack-profiler", 20, storage);
  if (!command_line->has_switch(command_line, &name)) return;
  if (command_line->base.size < 0xb0) {
    Log("libcef-shim: start-stack-profiler is on and this CEF cannot remove it\n");
    return;
  }
  command_line->remove_switch(command_line, &name);
  Log(command_line->has_switch(command_line, &name) ? "libcef-shim: start-stack-profiler is still on\n"
                                                   : "libcef-shim: removed start-stack-profiler\n");
}

static void AppendAndLog(cef_command_line_t* command_line) {
  AppendConfigured(command_line);
  if (!g_keep_profiler) RemoveStackProfiler(command_line);
  CheckSwitches("after appending", command_line);
  cef_string_userfree_free_fn free_string =
      (cef_string_userfree_free_fn)GetProcAddress(
          g_real, g_cef_utf8 ? "cef_string_userfree_utf8_free" : "cef_string_userfree_utf16_free");
  cef_string_t* line = command_line->get_command_line_string(command_line);
  if (line) {
    LogCefString("libcef-shim: command line now: ", line);
    LogHex("libcef-shim: command line starts with", line);
    if (free_string) free_string(line);
  }
}

static void FallbackAddRef(void* self) { (void)self; }
static int FallbackRelease(void* self) { (void)self; return 0; }
static int FallbackHasOneRef(void* self) { (void)self; return 1; }

static cef_app_t g_fallback_app;

__declspec(dllexport) int cef_initialize(const void* args, const void* settings, cef_app_t* app,
                                         void* sandbox_info) {
  static const WCHAR real_name[] = {'l', 'i', 'b', 'c', 'e', 'f', '_', 'r', 'e', 'a', 'l', '.', 'd', 'l', 'l', 0};
  g_real = LoadLibraryW(real_name);
  cef_initialize_fn real = g_real ? (cef_initialize_fn)GetProcAddress(g_real, "cef_initialize") : NULL;
  if (!real) {
    Log("libcef-shim: libcef_real.dll or its cef_initialize is missing\n");
    return 0;
  }
  LoadConfig();
  if (!app) {
    // The game gave no CefApp: give CEF one whose only job is the command line.
    g_fallback_app.base.size = sizeof(g_fallback_app);
    g_fallback_app.base.add_ref = (void*)FallbackAddRef;
    g_fallback_app.base.release = (void*)FallbackRelease;
    g_fallback_app.base.has_one_ref = (void*)FallbackHasOneRef;
    g_fallback_app.base.has_at_least_one_ref = (void*)FallbackHasOneRef;
    app = &g_fallback_app;
    Log("libcef-shim: the game passed no CefApp, using the shim's\n");
  }
  LogNumber("libcef-shim: CefApp struct size ", app->base.size);
  LogAddress("libcef-shim: BeamNG's handler ", (uint64_t)(uintptr_t)app->on_before_command_line_processing);
  LogAddress("libcef-shim: CefApp struct ", (uint64_t)(uintptr_t)app);
  if (g_no_hook) {
    Log("libcef-shim: shim:nohook set, passing cef_initialize through unchanged\n");
  } else {
    g_original_hook = app->on_before_command_line_processing;
    // The struct is normally heap memory; only change protection if it is not writable.
    struct { void* base; void* allocation; DWORD allocation_protect; size_t size; DWORD state, protect, type; } region;
    DWORD old_protect = 0;
    void* slot = &app->on_before_command_line_processing;
    BOOL writable = VirtualQuery(slot, &region, sizeof(region)) &&
                    (region.protect & 0xcc /* READWRITE | WRITECOPY | EXECUTE_READWRITE | EXECUTE_WRITECOPY */);
    LogNumber("libcef-shim: CefApp memory protection ", region.protect);
    if (writable || VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old_protect)) {
      app->on_before_command_line_processing = HookedOnBeforeCommandLineProcessing;
      if (!writable) VirtualProtect(slot, sizeof(void*), old_protect, &old_protect);
      Log("libcef-shim: hooked OnBeforeCommandLineProcessing\n");
    } else {
      Log("libcef-shim: could not hook OnBeforeCommandLineProcessing\n");
    }
  }
  // cef_settings_t: size, no_sandbox, three strings, then four ints (CEF 150).
  static _Alignas(16) unsigned char settings_copy[2048];
  const unsigned char* given = (const unsigned char*)settings;
  size_t settings_size = given ? *(const size_t*)given : 0;
  if (given && settings_size >= 104 && settings_size <= sizeof(settings_copy)) {
    for (size_t i = 0; i < settings_size; ++i) settings_copy[i] = given[i];
    int* ints = (int*)(settings_copy + 88);
    LogNumber("libcef-shim: settings size ", settings_size);
    LogNumber("libcef-shim: settings no_sandbox ", (uintptr_t)*(int*)(settings_copy + 8));
    LogNumber("libcef-shim: settings multi_threaded_message_loop ", (uintptr_t)ints[0]);
    LogNumber("libcef-shim: settings external_message_pump ", (uintptr_t)ints[1]);
    LogNumber("libcef-shim: settings windowless_rendering_enabled ", (uintptr_t)ints[2]);
    LogNumber("libcef-shim: settings command_line_args_disabled ", (uintptr_t)ints[3]);
    if (!g_keep_settings && ints[3]) {
      ints[3] = 0;  // let CEF read the switches from the game's own command line too
      Log("libcef-shim: cleared command_line_args_disabled\n");
    }
    settings = settings_copy;
  } else {
    LogNumber("libcef-shim: settings left alone, size ", settings_size);
  }
  Log("libcef-shim: calling the real cef_initialize\n");
  int result = real(args, settings, app, sandbox_info);
  LogNumber("libcef-shim: cef_initialize returned ", (uintptr_t)result);
  typedef cef_command_line_t* (*get_global_fn)(void);
  get_global_fn get_global = (get_global_fn)GetProcAddress(g_real, "cef_command_line_get_global");
  cef_command_line_t* global = get_global ? get_global() : NULL;
  if (global) {
    CheckSwitches("global command line after cef_initialize", global);
    ((int (*)(void*))global->base.release)(global);
  }
  if (g_dump_seconds) {
    LogNumber("libcef-shim: will list every thread's stack after seconds: ", g_dump_seconds);
    CreateThread(NULL, 0, (void*)DumpLater, NULL, 0, NULL);
  }
  return result;
}

#include "vk_hook.c"
#include "overlay.c"
#include "paint_hook.c"

BOOL WINAPI DllMain(HMODULE module, DWORD reason, void* reserved) {
  (void)module;
  (void)reserved;
  if (reason == DLL_PROCESS_ATTACH) {
    WCHAR path[1024];
    static const char name[] = "libcef_shim.log";
    DWORD n = ModuleDirectory(path, 1000);
    for (size_t i = 0; name[i]; ++i) path[n++] = (WCHAR)name[i];
    path[n] = 0;
    g_log_file = CreateFileW(path, 0x40000000 /* GENERIC_WRITE */, FILE_SHARE_READ, NULL, 2 /* CREATE_ALWAYS */,
                             0x80 /* FILE_ATTRIBUTE_NORMAL */, NULL);
    Log("libcef-shim: loaded (build 15)\n");
    AddVectoredExceptionHandler(1, (void*)ExceptionLogger);
    CreateThread(NULL, 0, (void*)HookVulkanLater, NULL, 0, NULL);
  }
  return 1;
}

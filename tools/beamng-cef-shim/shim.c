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
} cef_command_line_t;

typedef struct cef_app_t {
  cef_base_ref_counted_t base;
  void (*on_before_command_line_processing)(struct cef_app_t*, const cef_string_t* process_type,
                                            cef_command_line_t* command_line);
} cef_app_t;

typedef int (*cef_initialize_fn)(const void* args, const void* settings, cef_app_t* app, void* sandbox_info);
typedef void (*cef_string_userfree_free_fn)(cef_string_t*);

static const char* const kDefaultSwitches[] = {"single-process", "in-process-gpu", "no-sandbox", NULL};

static HMODULE g_real;
static void (*g_original_hook)(cef_app_t*, const cef_string_t*, cef_command_line_t*);
static BOOL g_no_hook;
static BOOL g_keep_settings;
static char g_config[4096];
static DWORD g_config_size;
static BOOL g_have_config;

static size_t Length(const char* s) {
  size_t n = 0;
  while (s[n]) ++n;
  return n;
}

static void Log(const char* text) {
  DWORD written;
  HANDLE err = GetStdHandle(STD_ERROR_HANDLE);
  if (err && err != INVALID_HANDLE_VALUE) WriteFile(err, text, (DWORD)Length(text), &written, NULL);
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
static long WINAPI ExceptionLogger(exception_pointers_t* info) {
  exception_record_t* record = info->record;
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
      static const char keep[] = "shim:keep-settings";
      i = 0;
      while (keep[i] && p[i] == keep[i]) ++i;
      if (!keep[i]) g_keep_settings = 1;
    }
  }
  CloseHandle(file);
}

static void Append(cef_command_line_t* command_line, const char* name, size_t name_length, const char* value,
                   size_t value_length) {
  WCHAR wide_name[256], wide_value[512];
  if (name_length >= 256 || value_length >= 512) return;
  for (size_t i = 0; i < name_length; ++i) wide_name[i] = (unsigned char)name[i];
  for (size_t i = 0; i < value_length; ++i) wide_value[i] = (unsigned char)value[i];
  cef_string_t cef_name = {wide_name, name_length, NULL};
  cef_string_t cef_value = {wide_value, value_length, NULL};
  if (command_line->has_switch(command_line, &cef_name)) {
    LogWide("libcef-shim: already set: ", wide_name, name_length);
    if (!value) return;
  }
  if (value)
    command_line->append_switch_with_value(command_line, &cef_name, &cef_value);
  else
    command_line->append_switch(command_line, &cef_name);
  LogWide("libcef-shim: appended ", wide_name, name_length);
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
    WCHAR wide[32];
    size_t length = Length(names[k]);
    for (size_t i = 0; i < length; ++i) wide[i] = (unsigned char)names[k][i];
    cef_string_t name = {wide, length, NULL};
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
    LogWide("libcef-shim: leaving a helper process alone: ", process_type->str, process_type->length);
  } else if (command_line->is_read_only(command_line)) {
    Log("libcef-shim: command line is read-only, nothing appended\n");
  } else {
    AppendAndLog(command_line);
  }
  ((release_fn)command_line->base.release)(command_line);
}

static void AppendAndLog(cef_command_line_t* command_line) {
  AppendConfigured(command_line);
  CheckSwitches("after appending", command_line);
  cef_string_userfree_free_fn free_string =
      (cef_string_userfree_free_fn)GetProcAddress(g_real, "cef_string_userfree_utf16_free");
  cef_string_t* line = command_line->get_command_line_string(command_line);
  if (line) {
    LogWide("libcef-shim: command line now: ", line->str, line->length);
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
  return result;
}

BOOL WINAPI DllMain(HMODULE module, DWORD reason, void* reserved) {
  (void)module;
  (void)reserved;
  if (reason == DLL_PROCESS_ATTACH) {
    Log("libcef-shim: loaded (build 4)\n");
    AddVectoredExceptionHandler(1, (void*)ExceptionLogger);
  }
  return 1;
}

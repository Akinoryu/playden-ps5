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

static void HookedOnBeforeCommandLineProcessing(cef_app_t* self, const cef_string_t* process_type,
                                                cef_command_line_t* command_line) {
  if (g_original_hook) g_original_hook(self, process_type, command_line);
  if (process_type && process_type->length) {
    LogWide("libcef-shim: leaving a helper process alone: ", process_type->str, process_type->length);
    return;
  }
  if (!command_line || command_line->is_read_only(command_line)) {
    Log("libcef-shim: command line missing or read-only, nothing appended\n");
    return;
  }
  AppendConfigured(command_line);
  cef_string_userfree_free_fn free_string =
      (cef_string_userfree_free_fn)GetProcAddress(g_real, "cef_string_userfree_utf16_free");
  cef_string_t* line = command_line->get_command_line_string(command_line);
  if (line) {
    LogWide("libcef-shim: command line now: ", line->str, line->length);
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
  g_original_hook = app->on_before_command_line_processing;
  DWORD old_protect;
  void* slot = &app->on_before_command_line_processing;
  if (VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old_protect)) {
    app->on_before_command_line_processing = HookedOnBeforeCommandLineProcessing;
    VirtualProtect(slot, sizeof(void*), old_protect, &old_protect);
    Log("libcef-shim: hooked OnBeforeCommandLineProcessing\n");
  } else {
    Log("libcef-shim: could not hook OnBeforeCommandLineProcessing\n");
  }
  int result = real(args, settings, app, sandbox_info);
  LogNumber("libcef-shim: cef_initialize returned ", (uintptr_t)result);
  return result;
}

BOOL WINAPI DllMain(HMODULE module, DWORD reason, void* reserved) {
  (void)module;
  (void)reserved;
  if (reason == DLL_PROCESS_ATTACH) Log("libcef-shim: loaded\n");
  return 1;
}

// SPDX-License-Identifier: GPL-3.0-or-later
//
// The game's watches for changes in folders, included by shim.c.
//
// The console has no inotify, so prospero-win's server finds what changed in a watched folder by
// listing it (its Wine patch 0884), with the whole tree under it when the watch takes in subfolders.
// It lists on its own thread, the one that answers every request of every thread of the game, and
// a listing that takes long is made less often: eight times as long between two. In build 24
// GetTickCount64, which that thread keeps, stood still for about 212 ms every 1.7 s while
// QueryPerformanceCounter went on, and each time a frame took about 240 ms: a listing of 212 ms,
// 8 x 212 ms apart, is the stutter. So a watch of a whole tree watches only its folder (shim:
// watch-subtrees keeps it as asked). On the console (build 25) that left no frame over 100 ms, and
// GetTickCount64 never stood still; BeamNG asks for two such watches at start-up and does not start
// when they are refused. Each folder is logged the first time it is watched, with what was asked,
// who asked and how long the call took: the server makes its first listing in it.

typedef long(WINAPI* notify_fn)(HANDLE, HANDLE, void*, void*, void*, void*, DWORD, DWORD, unsigned char);
typedef long(WINAPI* notify_ex_fn)(HANDLE, HANDLE, void*, void*, void*, void*, DWORD, DWORD, unsigned char, int);
typedef DWORD(WINAPI* final_path_fn)(HANDLE, WCHAR*, DWORD, DWORD);
static notify_fn g_real_notify;
static notify_ex_fn g_real_notify_ex;
static final_path_fn g_final_path;
static HANDLE volatile g_watch_seen[32];
static volatile long g_watch_logged;

// Whether this is the first watch call for the folder handle (as far as 32 handles go).
static BOOL WatchFirstTime(HANDLE folder) {
  for (int i = 0; i < 32; ++i) {
    HANDLE seen = g_watch_seen[i];
    if (seen == folder) return 0;
    if (!seen && __sync_bool_compare_and_swap(&g_watch_seen[i], NULL, folder)) return 1;
    if (g_watch_seen[i] == folder) return 0;
  }
  return 0;
}

// The first code address on this thread's stack, above this frame, in the game's program or in
// CEF: most likely the call that asked for the watch, through kernelbase. The stack is read by
// address, up to the end of its committed pages.
__attribute__((noinline)) static uint64_t WatchAsker(void) {
  struct { void* base; void* allocation; DWORD allocation_protect; size_t size; DWORD state, protect, type; } region;
  HMODULE program = GetModuleHandleW(NULL);
  uint64_t at = (uint64_t)(uintptr_t)__builtin_frame_address(0);
  if (!VirtualQuery((const void*)(uintptr_t)at, &region, sizeof(region))) return 0;
  uint64_t end = (uint64_t)(uintptr_t)region.base + region.size;
  if (end > at + 4096) end = at + 4096;
  for (; at + 8 <= end; at += 8) {
    uint64_t value = *(const volatile uint64_t*)(uintptr_t)at;
    const void* address = (const void*)(uintptr_t)value;
    if (!(program && InImage(program, address)) && !(g_real && InImage(g_real, address))) continue;
    if (VirtualQuery(address, &region, sizeof(region)) && (region.protect & 0xf0)) return value;
  }
  return 0;
}

static void WatchLog(HANDLE folder, DWORD filter, unsigned char asked, unsigned char watched, long status,
                     int64_t took, uint64_t through, uint64_t asker) {
  static char line[900];  // one at a time: watches start rarely
  static volatile long busy;
  if (__sync_lock_test_and_set(&busy, 1)) return;
  WCHAR name[300];
  DWORD length = g_final_path ? g_final_path(folder, name, 300, 2 /* VOLUME_NAME_NT */) : 0;
  size_t n = SampleAppend(line, 0, sizeof(line), "libcef-shim: watch: ");
  if (length && length < 300)
    for (DWORD i = 0; i < length && n + 1 < 400; ++i) line[n++] = name[i] < 0x80 ? (char)name[i] : '?';
  else
    n = SampleAppend(line, n, sizeof(line), "a folder whose name is unknown");
  n = SampleAppend(line, n, sizeof(line),
                   !asked    ? ", the folder alone"
                   : watched ? " and all below it, as asked"
                             : ": asked with all below it, watching the folder alone");
  n = SampleAppend(line, n, sizeof(line), "; changes ");
  n = HitchHex(line, n, sizeof(line), filter);
  if (status != 0x103 /* STATUS_PENDING */ && status) {
    n = SampleAppend(line, n, sizeof(line), "; status ");
    n = HitchHex(line, n, sizeof(line), (uint32_t)status);
  }
  n = SampleAppend(line, n, sizeof(line), "; the call took ");
  n = SampleNumber(line, n, sizeof(line), (uint64_t)(took * 1000 / g_qpc_frequency));
  n = SampleAppend(line, n, sizeof(line), " ms; through ");
  n = FormatAddress(line, n, sizeof(line), through);
  if (asker) {
    n = SampleAppend(line, n, sizeof(line), ", from ");
    n = FormatAddress(line, n, sizeof(line), asker);
  }
  SampleAppend(line, n, sizeof(line), "\n");
  Log(line);
  __sync_lock_release(&busy);
}

static long WINAPI WatchFolder(HANDLE folder, HANDLE event, void* apc, void* apc_context, void* io, void* buffer,
                               DWORD length, DWORD filter, unsigned char subtree) {
  unsigned char asked = subtree;
  if (subtree && !g_watch_subtrees) subtree = 0;
  BOOL log = WatchFirstTime(folder) && g_watch_logged < 24;
  int64_t start = 0, end = 0;
  if (log) RealQpc(&start);
  long status = g_real_notify(folder, event, apc, apc_context, io, buffer, length, filter, subtree);
  if (log) {
    RealQpc(&end);
    __sync_add_and_fetch(&g_watch_logged, 1);
    WatchLog(folder, filter, asked, subtree, status, end - start, (uint64_t)(uintptr_t)__builtin_return_address(0),
             WatchAsker());
  }
  return status;
}

static long WINAPI WatchFolderEx(HANDLE folder, HANDLE event, void* apc, void* apc_context, void* io, void* buffer,
                                 DWORD length, DWORD filter, unsigned char subtree, int information) {
  unsigned char asked = subtree;
  if (subtree && !g_watch_subtrees) subtree = 0;
  BOOL log = WatchFirstTime(folder) && g_watch_logged < 24;
  int64_t start = 0, end = 0;
  if (log) RealQpc(&start);
  long status = g_real_notify_ex(folder, event, apc, apc_context, io, buffer, length, filter, subtree, information);
  if (log) {
    RealQpc(&end);
    __sync_add_and_fetch(&g_watch_logged, 1);
    WatchLog(folder, filter, asked, subtree, status, end - start, (uint64_t)(uintptr_t)__builtin_return_address(0),
             WatchAsker());
  }
  return status;
}

// From DllMain: the calls that start a watch, as kernelbase (ReadDirectoryChangesW,
// FindFirstChangeNotificationW) and kernel32 make them, and the program if it calls ntdll itself.
// The real calls are found before any import points here.
static void HookFolderWatches(void) {
  static const WCHAR ntdll_name[] = {'n', 't', 'd', 'l', 'l', '.', 'd', 'l', 'l', 0};
  static const WCHAR kernelbase[] = {'k', 'e', 'r', 'n', 'e', 'l', 'b', 'a', 's', 'e', '.', 'd', 'l', 'l', 0};
  static const WCHAR kernel32[] = {'k', 'e', 'r', 'n', 'e', 'l', '3', '2', '.', 'd', 'l', 'l', 0};
  HMODULE ntdll = GetModuleHandleW(ntdll_name);
  HMODULE modules[3] = {GetModuleHandleW(kernelbase), GetModuleHandleW(kernel32), GetModuleHandleW(NULL)};
  if (!g_qpc_frequency) QueryPerformanceFrequency(&g_qpc_frequency);
  if (modules[1]) g_final_path = (final_path_fn)GetProcAddress(modules[1], "GetFinalPathNameByHandleW");
  if (ntdll) {
    g_real_notify = (notify_fn)GetProcAddress(ntdll, "NtNotifyChangeDirectoryFile");
    g_real_notify_ex = (notify_ex_fn)GetProcAddress(ntdll, "NtNotifyChangeDirectoryFileEx");
  }
  int patched = 0;
  for (int i = 0; i < 3; ++i) {
    if (!modules[i]) continue;
    if (g_real_notify && PatchImport(modules[i], "NtNotifyChangeDirectoryFile", (void*)WatchFolder)) ++patched;
    if (g_real_notify_ex && PatchImport(modules[i], "NtNotifyChangeDirectoryFileEx", (void*)WatchFolderEx)) ++patched;
  }
  Log(!patched           ? "libcef-shim: watch: folder watches could not be hooked; they are as asked\n"
      : g_watch_subtrees ? "libcef-shim: watch: folder watches are logged and kept as asked\n"
                         : "libcef-shim: watch: folder watches are logged; a watch of a whole tree watches only its "
                           "folder\n");
}

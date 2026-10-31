# CEF in-process shim (experimental)

To set up BeamNG.drive on a PS5 with this shim, and for the story of each problem it fixes, see
[GUIDE.md](GUIDE.md). This file is the technical reference.

BeamNG.drive draws its interface with CEF (Chromium). Chromium starts helper processes for the GPU,
network and storage services and for each page, and a PS5 title cannot start processes, so BeamNG
stops a second or so after CEF starts. This shim is a `libcef.dll` stand-in that forwards every export
to the real library, renamed `libcef_real.dll`, and appends switches that ask Chromium to do that work
inside the game's process (`--single-process --in-process-gpu --no-sandbox` by default).

By default it also caps Chromium's shared memory (`--enable-low-end-device-mode
--force-gpu-mem-available-mb=64`). With GPU rendering off, the UI's tiles and decoded images live
in shared memory, which on the PS5 comes out of the title's ~440 MiB of flexible memory, while
Chromium's desktop budgets allow 512 MiB of each. When flexible memory runs out the system closes
the title: BeamNG went back to the home screen from a menu, the runtime's last `PW_WINE64 memory`
lines showing 10 to 21 MiB of it free. The UI's share grows with BeamNG's size: in Window 1280x720
about 215 MiB stayed free while driving, while in Fullscreen 1920x1080 the main menu left 35 to
90 MiB and opening Options used the rest (Chromium's out-of-memory exception `0xe0000008`, which
the shim logs with the other crashes).

At 1920x1080 menus and loading screens also showed black squares where 256 KiB tiles could not be
mapped, and the game closed in menus, Options and loading screens. So when BeamNG's view is wider
than 1280 pixels, the shim (`ui_memory.c`) lowers the device scale factor BeamNG reports to CEF
(`GetScreenInfo`, which CEF asks again at each resize) by 1280 / the view's width. Chromium lays the
page out at BeamNG's size as before and keeps mouse positions in it, but draws it 1280 pixels wide,
with the tiles of 1280x720. Before BeamNG gets each frame (`OnPaint`), the shim scales the parts
that changed up to the size BeamNG expects (a `<select>` list's popup to the size `OnPopupSize`
gave), into a frame of its own that it keeps between paints: 1280x720 to 1920x1080 in blocks of two
pixels to three (a whole frame took about 1.3 ms on a 2.8 GHz Xeon), other sizes bilinear. The
interface looks a little softer; the 3D picture keeps its size. `shim:ui-width=N` draws N pixels
wide instead (640 or more), and `shim:ui-full` at the view's size, as before build 28. Every 10
seconds a `ui:` line gives the shared memory CEF has mapped (`MapViewOfFile` from
`libcef_real.dll`): its views and MiB, the most so far, and how many views could not be mapped; the
first eight of those are logged as they happen. BeamNG's vehicle screens (odometers, navigation,
gauges) are browsers of their own, with the same render handler and views of 1024x512 or smaller;
CEF keeps a scale for each browser, and the shim decides for each frame, from its browser's view,
whether it was drawn smaller. Build 28 kept the last scale it gave instead, which a vehicle screen
reset: the main view's frames then reached BeamNG at 1280x720. Not yet tested on a console.

Chromium makes most of its threads without a stack size, so they take the program's default, which
chrome.exe sets to 8 MiB. In BeamNG they took BeamNG's, and the renderer's thread, which runs inside
the game in single-process mode, overflowed it when the Options page laid out (`0xc00000fd`). The
shim patches `libcef_real.dll`'s `CreateThread` import so those threads get 8 MiB (or the program's
default if that is more) of address space; the runtime commits a stack's pages only as they are used.

It needs no runtime or Playden change. Chromium does not support single-process mode officially, so
this may only move the failure.

## Build

```sh
# on Windows: list the exports of the game's own libcef.dll
powershell -ExecutionPolicy Bypass -File dump-exports.ps1
# here: clang, lld-link and llvm-dlltool
tools/beamng-cef-shim/build.sh libcef-exports.txt out/
```

## Install (PS5, over FTP)

In `/data/playden-games/284160/Bin64/`:

1. Rename `libcef.dll` to `libcef_real.dll`.
2. Upload the built `libcef.dll`.
3. Optional: upload `libcef_shim.txt` with one switch per line (`name` or `name=value`, `#` comments)
   to replace the default switches. Lines starting with `shim:` are options for the shim itself; a
   file with only those keeps the default switches.

With Playden's kit 0.15 the shim also watches the game's Vulkan swapchains through winevulkan's
Unix call dispatcher. On the PS5, swapchains of one size share their display buffers, and BeamNG
replaces its swapchain twice at startup; if the old ones still hold an acquired image (the likely
cause, not yet confirmed on the console), its third swapchain waits forever for a free buffer
(black screen, sound playing). The shim presents
those leftover images on the replaced swapchains, which frees their buffers. `shim:no-vk-fix`
keeps the log and turns that off. A Mesa fix in PS5_Mesa's `wsi_common_videoout.c` would make
this unnecessary.

## Menu overlay and full-size swapchain

On the console CEF draws BeamNG's menus correctly, yet the screen stayed black. With
`shim:menu-copy`, while CEF's latest frame is opaque (loading screens, menus), the shim copies it
into each swapchain image just before BeamNG presents it (`overlay.c`); see-through frames (the
in-game HUD) leave BeamNG's frame alone. Since build 27 that copy is off by default: the black
screen came from the size (below), and BeamNG draws its menus itself into the full-size swapchain
(build 25 showed them with no copy). `shim:no-overlay` turns the overlay off altogether.

With that the screen was still black: BeamNG's window is 1280x720, a size the PS5's VideoOut does
not scan out, so Mesa gave its swapchain images of its own and blitted each into a 1920x1080
framebuffer at present. The shim therefore makes BeamNG's swapchain 1920x1080, VideoOut's own
framebuffers, while BeamNG keeps drawing 1280x720 at the top left of each image, and before each
present it scales that part (or the menu) up over the whole image, aspect ratio kept. Wine reports
such a swapchain as suboptimal, since it is not the window's size; the shim reports success to
BeamNG instead, or it would make its swapchain again at every frame. If the full size cannot be
made, the shim makes BeamNG's size as before. `shim:no-vk-fullsize` keeps BeamNG's size, and
`shim:vk-size=3840x2160` picks another full size. With this, BeamNG's menus and its 3D world show on
the console.

When BeamNG changes its size (it applies its display settings at start-up, and again from
Options), it destroys its swapchain and makes another, and the driver can hand the new one the
destroyed one's handle. Up to build 25 the overlay then kept the old size and images: after
BeamNG went from 1280x720 to Fullscreen 1920x1080 it scaled frames that needed none, into images
that were not the ones being shown, and the picture shook between two sizes until a restart. The
overlay now forgets every swapchain the shim destroys and reads each new one's size and images.

A window of BeamNG's own beside its main one (some debug pop-ups, such as the Performance Graph's
Export) gets no swapchain on the PS5: every window shows on the one display, and Wine answers
`VK_ERROR_NATIVE_WINDOW_IN_USE_KHR`. BeamNG asked ten times and then crashed. Such a window now
gets a stand-in (`standin.c`): images of the shim's own that BeamNG draws into and presents, and
that nothing shows. An acquire hands out the next image at once; the semaphore and fence it should
signal are signalled by an empty submit just before BeamNG's next submit, on that queue. A present
passes the main window's swapchain on and drops the stand-in, and when only stand-ins are left an
empty submit waits on its semaphores instead. Debug names and HDR metadata for a stand-in stay in
the shim. The pop-up itself is not shown.

`test/run.sh` checks the overlay and the scaling, and a stand-in used beside a real swapchain, on
real Vulkan (Mesa's lavapipe under Wine, with the Khronos validation layer and its synchronization
checks), and the interface's scaling (`test/ui_memory_test.c`, built for the host).

The shim logs each step to stderr as `libcef-shim: ...` lines in the title log, and to
`libcef_shim.log` beside the DLL (Playden keeps only the start and end of a long title log). Every
10 seconds a `pace:` line counts the frames presented, how many took over 17, 34 and 100 ms, and how
long each frame waited in acquire (for a free display buffer) and spent in present. A second `pace:`
line splits each frame of the thread that submits the game's work to the GPU (in BeamNG not the one
that presents): waiting for the GPU (fences, semaphores, idle queues), submitting, polling fences,
the other Vulkan calls (recording, mostly), and what is left, the game's own work; it also counts
the Vulkan calls of other threads. The calls are timed with rdtsc when it is cheap; a `clocks:` line
at start says what it and `QueryPerformanceCounter` cost. A third `pace:` line lists the frames over
100 ms, each with when it began in seconds since the shim loaded (BeamNG's own log counts from about
then) and how long it took, and a fourth the frames of 34 to 100 ms (two refreshes or more) the
same way. Every 10 seconds a `cef_do_message_loop_work` line gives how long BeamNG's calls into
CEF took on its main thread, and a `pace:` line lists the calls over 8 ms, to set beside the slow
frames.

On the console BeamNG stops for about 250 ms every 1.7 seconds, in menus and while driving, and runs
at 60 frames a second in between (build 22). To see who holds it up, a watchdog (`hitch.c`) notices
when a frame is 120 ms late in otherwise smooth play, from a minute after start, stops every thread
of the game at once, notes where each is, lets them all go, and logs a `hitch` line and then a line
per thread: its name, where it was (with the function for Windows' and Wine's modules) and the code
addresses on its stack. It does this for six stalls, 5 seconds apart at least, and the line says
how long the threads were held. It also times `cef_do_message_loop_work`, which the game's main
thread calls each frame, says whether the main thread was inside it during the stall, and logs every
10 seconds how long those calls took. `shim:no-hitch` turns it off.

The frames of 34 to 100 ms left in build 27 (about six in each 10 s) came at a steady rhythm: one of
about 36 ms with each call into CEF of about 25 ms, every 5.01 s, and two of about 49 ms in the same
5.01 s, 2 s apart, outside CEF. Every 5 seconds Chromium's memory pressure check
(`SystemMemoryPressureEvaluator`) calls `GlobalMemoryStatusEx` on its browser thread, which in
BeamNG is the game's main thread, so the shim answers `libcef_real.dll`'s calls (`memory_check.c`)
with Windows' last answer while it is under a minute old, logs the first with what Windows said and
any that took over 5 ms, and every 10 seconds how many calls there were. `shim:no-memory-cache`
passes them all on. For the others the watchdog also stops the main thread and the ones that submit
and present when a frame is 30 ms late in smooth play, and logs them as `slow frame` lines the same
way: twelve frames, 15 seconds apart at least. Each stop makes its frame longer by about as long as
it takes to land.

On the console (build 29) `GlobalMemoryStatusEx` took 24 to 25 ms: Wine works out the process's
memory counters (`NtQueryInformationProcess`, `ProcessVmCounters`) there from every mapping. With
the cache the frames of about 36 ms were gone. The frames of about 50 ms still came, every 2 s on
Small Island, and in one of them BeamNG's main thread was in `GetProcessMemoryInfo`, which asks for
the same counters. So build 30 answers `GetProcessMemoryInfo` and `K32GetProcessMemoryInfo` for the
game's own process, and `GlobalMemoryStatusEx`, the same way for `BeamNG.drive.x64.exe` and
`mimalloc.dll` as for `libcef_real.dll`.

In each stall it listed (build 23), the game's main thread was in `Sleep`, or reading
`QueryPerformanceCounter`, in `std::this_thread::sleep_for`, its wait between frames. So the shim
also wraps the game's `Sleep` and `QueryPerformanceCounter` imports (`time_hook.c`): every 10
seconds a `sleep:` line counts the main thread's sleeps, what they asked for and took, and the
longest with where it came from, the other threads' sleeps and the clock reads, and how many times
`GetTickCount64` stood still for over 50 ms; a main-thread sleep that took 50 ms more than it asked
for is logged as it happens, and so is a 2 ms sleep of a thread of the shim's own that took over
50 ms, or over which `GetTickCount64` and `QueryPerformanceCounter` disagree by as much. With
`shim:yield-sleep`, the main thread's sleeps of 1 to 4 ms wait by yielding the CPU instead.

Build 24 found the game's sleeps on time, and `GetTickCount64` standing still for about 212 ms in
each stall while `QueryPerformanceCounter` went on. The console has no inotify, so prospero-win's
server finds what changed in a folder a program watches by listing it, with the whole tree under it
when the watch takes in subfolders. It lists on its own thread, which keeps that tick and answers
every request of every thread of the game, and a listing that takes long is made less often: eight
times as long between two. One listing of about 212 ms every 8 x 212 ms is the stutter. So the shim
hooks the call that starts a watch (`NtNotifyChangeDirectoryFile`, as kernelbase calls it for
`ReadDirectoryChangesW` and `FindFirstChangeNotificationW`; `watch_hook.c`): a watch of a whole tree
watches only its folder, and changes further down are not reported. Each folder is logged the first
time it is watched, in a `watch:` line with what was asked, how long the call took (the server makes
its first listing in it) and who asked. `shim:watch-subtrees` keeps watches as asked.

On the console (build 25) BeamNG asked for two such watches at start-up, both now watching their
folder alone, and the stutter was gone: while driving, about 60 frames a second and no frame over
100 ms, and `GetTickCount64` never stood still. Refusing those watches (build 25's
`shim:no-watch`) keeps BeamNG from starting, so that switch is gone.

With `shim:sample`, for 30 seconds of every minute, ten times a second, the shim also stops the
thread that submits for a moment and notes where it is (`sampler.c`): after each 30 seconds a
`sample:` line lists the modules (and for Windows' and Wine's, the function) it was found in most,
with the Vulkan call when it was in one, and a second line the game modules that called into
Windows, Wine and Vulkan. On the console a stop held the thread 11 to 15 ms, so it is off unless
asked for.

Every half minute the shim also copies BeamNG's own log, `beamng.log` in the game's user folder
inside the bottle (which FTP cannot read), to `libcef_shim_beamng.log` beside the DLL.
`shim:save-frames` saves CEF frames 20, 100, 300 and 600 as `libcef_ui_frame_<n>.bmp`, and
`shim:dump-after=N` lists every thread's stack N seconds after CEF starts, twice; both hold up the
game while they run, so they are off unless asked for.

To undo, delete the shim and rename `libcef_real.dll` back. A Steam update or file check puts the
original back.

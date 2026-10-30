# CEF in-process shim (experimental)

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
lines showing 10 to 21 MiB of it free.

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

On the console CEF draws BeamNG's menus correctly, yet the screen stayed black. While CEF's
latest frame is opaque (loading screens, menus), the shim copies it into each swapchain image just
before BeamNG presents it (`overlay.c`); see-through frames (the in-game HUD) leave BeamNG's frame
alone. `shim:no-overlay` turns it off.

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

`test/run.sh` checks the overlay and the scaling on real Vulkan (Mesa's lavapipe under Wine, with
the Khronos validation layer and its synchronization checks).

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
then) and how long it took.

On the console BeamNG stops for about 250 ms every 1.7 seconds, in menus and while driving, and runs
at 60 frames a second in between (build 22). To see who holds it up, a watchdog (`hitch.c`) notices
when a frame is 120 ms late in otherwise smooth play, from a minute after start, stops every thread
of the game at once, notes where each is, lets them all go, and logs a `hitch` line and then a line
per thread: its name, where it was (with the function for Windows' and Wine's modules) and the code
addresses on its stack. It does this for six stalls, 5 seconds apart at least, and the line says
how long the threads were held. It also times `cef_do_message_loop_work`, which the game's main
thread calls each frame, says whether the main thread was inside it during the stall, and logs every
10 seconds how long those calls took. `shim:no-hitch` turns it off.

In each stall it listed (build 23), the game's main thread was in `Sleep`, or reading
`QueryPerformanceCounter`, in one function that waits for a time to come, while every other thread
was idle: BeamNG was waiting for time to pass, not for work. So the shim also wraps the game's `Sleep`
and `QueryPerformanceCounter` imports (`time_hook.c`): every 10 seconds a `sleep:` line counts the main
thread's sleeps, what they asked for and took, and the longest with where it came from, the other
threads' sleeps and the clock reads; a main-thread sleep that took 50 ms more than it asked for is
logged as it happens, and so is a 2 ms sleep of a thread of the shim's own that took over 50 ms. The
main thread's sleeps of 1 to 4 ms wait by yielding the CPU until the time has come, which needs no
timer (`shim:real-sleep` turns that off). Half a minute after start it logs the code of the waiting
function and of its callers in hex, with the game's imports that have to do with time, to read what
it waits for.

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

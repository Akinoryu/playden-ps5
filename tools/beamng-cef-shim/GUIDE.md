# BeamNG.drive on PS5 with Playden

BeamNG.drive does not run on Playden as it is: its interface is Chromium (CEF), and Chromium needs things a PS5
title cannot do. This folder holds a small replacement for BeamNG's `libcef.dll` (the "shim") that works around
each of those problems. With it, BeamNG starts, shows its menus, and drives smoothly on the PS5.

This guide explains how to set it up, what each part does and why, and what is still missing. For the technical
reference, see [README.md](README.md).

## Status

On the console it was tested on, BeamNG works:

- the game starts, and the loading screens, the main menu, the Options menu and the in-game HUD all show;
- the game fills the TV at 1920x1080;
- while driving it runs at about 60 frames a second, with no frame over 100 ms.

It was tested with:

| | |
| --- | --- |
| Console | PS5, firmware 13.60 |
| Playden | 0.9.0-beta.2 with runtime kit 0.15. Playden 0.9.0 uses the same kit, but was not tested. |
| Game | BeamNG.drive 0.40.4.0 (build 296339), from Steam (app 284160) |
| BeamNG's CEF | 150.0.20 (the export list in [exports-cef-150.0.20.txt](exports-cef-150.0.20.txt)) |

It is one console and one game version. Other firmwares, kits and BeamNG versions may behave differently.

## What you need

- A jailbroken PS5 running Playden, signed in to Steam, with BeamNG.drive downloaded.
- FTP access to the console.
- The shim's `libcef.dll`. Build it as described in [Building](#building), or use a prebuilt one if it is attached
  where you found this guide.

## Setup

### 1. Playden's game settings

Open BeamNG.drive's **Game settings** in Playden and set:

| Setting | Value |
| --- | --- |
| **Program** | `Bin64/BeamNG.drive.x64.exe` |
| **Extra arguments** | `-gfx vk --single-process --in-process-gpu --no-sandbox --disable-gpu --disable-gpu-compositing --disable-stack-profiler` |
| **Advanced › Library overrides** | `crashrpt=d` |

Why each one is there:

- **Program.** The default program is BeamNG's launcher. It fails with error 50 (request not supported), because
  it tries to start the game as a new program. `BeamNG.drive.x64.exe` is the game itself.
- **`-gfx vk`** picks BeamNG's Vulkan renderer, the same as "Start (Vulkan renderer)" on a PC. Without it, BeamNG
  picks Direct3D 12 and closes within a few seconds. The shim's display fixes are written for Vulkan.
- **`--disable-gpu --disable-gpu-compositing`** keep Chromium drawing in software. Without them one of Chromium's
  GPU threads crashed the game. The shim does not add these two, so they must stay here.
- **`--single-process --in-process-gpu --no-sandbox --disable-stack-profiler`**: the shim adds these by itself.
  Having them here as well does no harm.
- **`crashrpt=d`** stops BeamNG's crash reporter from loading. The reporter tries to start a helper program, fails,
  unloads itself, and then the game crashed calling into it. BeamNG's log then says it could not install the crash
  reporter. That message is expected.

### 2. Install the shim (FTP)

In `/data/playden-games/284160/Bin64/`:

1. Rename `libcef.dll` to `libcef_real.dll`. Keep this file: the shim passes everything to it.
2. Upload the shim's `libcef.dll` in its place.
3. Do not upload a `libcef_shim.txt` unless you need one of the [options](#options). The defaults are the tested
   setup.

### 3. BeamNG's own settings

These are the settings it was tested with:

- **Display mode:** Window, 1280x720. The shim scales the picture to fill the TV, so Fullscreen at the same size
  would look and run the same. A lower 16:9 size (for example 960x540) should run faster and look softer; that was
  not tested.
- **Graphics:** the lowest preset.
- **FPS limit:** off. A 30 fps limit made the frame pacing worse.

### Updating and undoing

- **A Steam update or file check of BeamNG puts the original `libcef.dll` back.** Repeat step 2 afterwards.
- If BeamNG updates its CEF version, the shim must be rebuilt with the new version's export list (see
  [Building](#building)). An old shim can stop the game from starting.
- **To undo**, delete the shim's `libcef.dll` and rename `libcef_real.dll` back to `libcef.dll`.

## What went wrong, and how each problem was fixed

These are the problems in the order they showed up. Each is fixed in the shim, so the setup above includes all of
the fixes.

### 1. BeamNG closed about 10 seconds after starting

Chromium starts helper programs for its GPU, network and storage work and for each page. A PS5 title cannot start
programs, so BeamNG's interface failed and took the game down with it.

**Fix:** the shim stands in for `libcef.dll`, forwards every call to the real one (`libcef_real.dll`), and asks
Chromium to do all that work inside the game: `--single-process --in-process-gpu --no-sandbox`. Getting those
switches to work needed three more fixes:

- **BeamNG tells CEF to ignore the game's command line** (`command_line_args_disabled`), so Extra arguments never
  reached Chromium. The shim clears that setting.
- **BeamNG's CEF stores text as UTF-8,** so the switches the shim first wrote (in UTF-16) were garbage. The shim
  reads CEF's string type and builds its strings to match.
- **The shim used CEF's command line after BeamNG had released it,** and CEF stopped the game. The shim now holds
  its own reference while it adds the switches.

### 2. Crashes in the crash reporter, in Chromium's GPU, and in its stack profiler

Three crashes came next:

- **The crash reporter:** fixed by `crashrpt=d`, as above.
- **Chromium's GPU threads:** fixed by drawing in software, with `--disable-gpu --disable-gpu-compositing`.
- **Chromium's stack sampling profiler:** BeamNG turns it on, and it reads other threads' stacks. On the PS5 that
  ended the game with a read fault at `0x1000114000`. The shim removes BeamNG's `--start-stack-profiler` and adds
  `--disable-stack-profiler`. If such a read still lands on a page that is reserved but not yet committed, the shim
  commits that page and lets the read run again.

### 3. A black screen with the music playing

While starting up, BeamNG builds its swapchain (the set of images it shows on screen) three times. Each time, it
destroys the old one while still holding one of its images. On the PS5, swapchains of the same size share the same
three display buffers. After the third swapchain, no buffer was ever free, and BeamNG waited forever for one.

**Fix:**

- The shim keeps a destroyed swapchain and hands it back when BeamNG asks for an identical one.
- When a wait for a buffer gets stuck, it presents the images left on replaced swapchains, which frees their
  buffers.

To see BeamNG's Vulkan calls at all, the shim hooks Wine's Vulkan layer (`winevulkan.dll`) at the point where it
calls into the PS5 side. This needed care:

- the game loads more than one copy of that DLL, and each copy is hooked;
- the hook is checked every 100 ms and put back if something restores the original;
- at startup a copy can still hold Wine's startup stub; calls are then sent straight to the real dispatcher, so the
  stub cannot undo the hook. Missing this once left the picture unscaled in a corner of the screen.

### 4. The menus did not draw

CEF handed BeamNG one empty frame and then no more. BeamNG asks CEF to draw only when told to ("external begin
frames"). In this single-process, software-drawn setup, CEF never got the next request.

**Fix:** the shim creates BeamNG's browsers without external begin frames, so CEF draws at its own rate. CEF then
drew the loading screens and the main menu correctly. Copies of its frames were saved as bitmaps to check this.

### 5. Still a black screen, with correct menus and a running game

BeamNG's own drawing of the menu never reached the screen. Then, even with the menu copied in, the picture stayed
black. BeamNG's 1280x720 window is a size the PS5's video output does not show directly, and the resize step on the
way to the 1920x1080 screen lost the picture.

**Fix:**

- **Menus:** while CEF's latest frame is opaque (loading screens and menus), the shim copies it into each swapchain
  image just before BeamNG presents it. See-through frames, such as the in-game HUD, leave the game's own picture
  alone.
- **Screen size:** the shim gives BeamNG a full 1920x1080 swapchain. BeamNG keeps drawing 1280x720 in its top-left
  corner, and before each present the shim scales that part (or the menu) to fill the screen, keeping the aspect
  ratio.
- **Swapchain warnings:** Wine calls such a swapchain "suboptimal" because it is not the window's size. The shim
  reports success instead, or BeamNG would rebuild its swapchain on every frame.

This is checked on a PC with real Vulkan (Mesa's lavapipe) and the Khronos validation layer by
[test/run.sh](test/run.sh).

### 6. Back to the PS5 home screen from a menu

The game closed with no error in any log. On the PS5, Windows shared memory comes out of a game's pool of about
440 MiB of "flexible" memory, and the system closes a game that runs out of it. With GPU drawing off, Chromium keeps
the interface's tiles and decoded images in shared memory, and its desktop limits allow 512 MiB of each. Just
before each close, the logs showed 10 to 21 MiB of that memory left.

**Fix:** the shim adds `--enable-low-end-device-mode` (64 MiB for images) and `--force-gpu-mem-available-mb=64`
(for tiles).

### 7. Opening Options crashed the game

The crash was a stack overflow (`0xc00000fd`) in Chromium's renderer thread. In single-process mode that thread runs
inside the game, with the game's default stack (1 MiB). Chrome itself gives its threads 8 MiB, and the Options page
needs more than 1 MiB to lay out.

**Fix:** the shim gives every thread that Chromium creates without a size 8 MiB of address space. The runtime only
uses memory for the pages a thread actually touches.

### 8. A freeze of about a quarter second every 1.7 seconds

Even at 60 frames a second, the game froze for about 230-280 ms every 1.7 s, in menus and while driving. It took
several rounds of measuring to find out why:

- **Nothing in the game itself took that long.** No GPU wait did, and the game's own sleeps were on time.
- During every freeze, **`GetTickCount64` stood still for about 212 ms** while the precise clock
  (`QueryPerformanceCounter`) kept going. That millisecond clock is kept by the Wine server's thread, which also
  answers every request of every thread of the game.
- The PS5 has no way to tell Wine that a file changed (no inotify). So when a program asks to watch a folder for
  changes, Wine on the PS5 **lists the folder over and over, and with it every folder below it.** It does this on
  that server thread, and it waits 8 times as long as the last listing took before listing again.
- BeamNG asks for **two such whole-tree watches** at startup. One full listing took about 212 ms, and
  8 x 212 ms is 1.7 s: exactly the stutter.

**Fix:** the shim hooks the call that starts a watch (`NtNotifyChangeDirectoryFile`, which `ReadDirectoryChangesW`
and `FindFirstChangeNotificationW` use). It makes a watch of a whole tree cover only its folder. Each listing then
takes under 1 ms, and the freezes are gone: about 60 frames a second while driving, no frame over 100 ms, and the
millisecond clock never stopped again. The one side effect: while the game runs, it does not notice files that
change in subfolders of those folders (for example a mod copied in over FTP). Restart the game to pick them up.

Refusing the watches entirely instead (an option in one test build) kept BeamNG from starting, so that option was
removed.

The real cause is in the PS5 Wine runtime ([prospero-win](https://github.com/mpereiraesaa/prospero-win), its patch
`0884-server-ps5-poll-for-directory-changes`). It affects any game that watches a large folder tree, not only
BeamNG. A fix there would make this part of the shim unnecessary.

### Along the way

- The shim got lighter. Saving CEF frames and listing every thread's stack are now off unless asked for (each held
  up the game). The menu copy runs only for frames it will draw.
- BeamNG's own log lives inside the game's private Windows folder (the "bottle"), which FTP cannot read. The shim
  copies it every 30 seconds to `libcef_shim_beamng.log` beside the DLL.

## Logs

The shim writes to three places:

- **`libcef_shim.log`** in `Bin64/`, rewritten at each start.
- **`libcef_shim_beamng.log`** in `Bin64/`: BeamNG's own log, copied every 30 seconds.
- **Playden's title log**, as `libcef-shim:` lines. To collect Playden's logs, run
  `python3 tools/collect_logs.py --host <PS5 IP>` from a computer. Playden keeps only the start and end of a long
  title log, so the two files above are the complete record.

Useful lines in `libcef_shim.log`:

| Line | What it says |
| --- | --- |
| `loaded (build N)` | Which shim build is running. |
| `watch: ... watching the folder alone` | A whole-tree watch was narrowed (the stutter fix). |
| `pace: N frames, ... over 100 ms ...` | Every 10 s: frames presented, how many were slow, and where frame time went. |
| `pace: frames over 100 ms, at seconds (ms): ...` | When each long frame happened and how long it took. |
| `sleep: ... GetTickCount64 stood still N times ...` | Every 10 s. Anything above 0 means the Wine server is stalling. |
| `overlay ...` / `vk ...` | The menu copy and the Vulkan hook at work. |
| `exception code ...` | A crash or C++ exception, with its call stack. |

## Options

Put options in a `libcef_shim.txt` next to the DLL, one per line. Lines starting with `shim:` change the shim
itself. A file with only `shim:` lines keeps the default Chromium switches. Any other line is a Chromium switch, and
a file with any such line replaces all of the default switches. None of these options are needed for normal play.

| Option | What it does |
| --- | --- |
| `shim:watch-subtrees` | Keep whole-tree folder watches as asked (brings the stutter back). |
| `shim:no-overlay` | Do not copy CEF's menu frames into the game's picture. |
| `shim:no-vk-fullsize` | Keep BeamNG's own swapchain size instead of 1920x1080. |
| `shim:vk-size=WxH` | Use another full size, for example `3840x2160` (not tested). |
| `shim:no-vk-fix` | Do not free display buffers held by replaced swapchains. |
| `shim:no-vk-reuse` | Do not hand back a destroyed swapchain for an identical one. |
| `shim:keep-begin-frame` | Keep BeamNG's external begin frames (the menus then stop drawing). |
| `shim:no-shared-texture` | Create CEF's browsers without shared textures. |
| `shim:keep-settings` | Keep BeamNG's `command_line_args_disabled` setting. |
| `shim:keep-profiler` | Keep Chromium's stack profiler (it crashed the game). |
| `shim:no-commit` | Do not commit reserved pages on read faults. |
| `shim:no-hitch` | Turn off the watchdog that lists threads when a frame stalls. |
| `shim:sample` | Sample where the rendering thread spends its time (costly, for debugging). |
| `shim:yield-sleep` | Make the game's short sleeps on its main thread yield the CPU instead (for debugging). |
| `shim:save-frames` | Save CEF frames 20, 100, 300 and 600 as `libcef_ui_frame_<n>.bmp` (holds up the game). |
| `shim:dump-after=N` | List every thread's stack N seconds after CEF starts (holds up the game). |
| `shim:nohook` | Do not add or remove any Chromium switches; Extra arguments still reach Chromium (for comparison only). |

## Troubleshooting

- **The game does not open.**
  - Check that `libcef_real.dll` exists in `Bin64/`.
  - Delete `libcef_shim.txt` if you have one.
  - Check that the shim was built for your BeamNG version.
- **The game freezes for a moment every 1.7 seconds.** You are on a shim older than build 25. Use the latest.
- **Opening Options crashes the game.** You are on a shim older than build 22. Use the latest.
- **A black screen with sound.** Check that `-gfx vk` is in Extra arguments, and look for `vk` and `overlay` lines in
  `libcef_shim.log`.
- **The picture fills only the top-left of the screen.** Look for `overlay` lines in the log, and use the latest
  shim.
- **The game goes back to the home screen from a menu.** Check that you have no `libcef_shim.txt` that replaces the
  default switches: they include the memory limits.
- **Anything else:** collect the logs (see [Logs](#logs)) and report them with your firmware, Playden, kit and
  BeamNG versions.

## Known limits

- **Small hitches remain:** about 6 frames in every 10 s take 35-50 ms instead of 17 ms. They were not investigated.
- **Heavier scenes may be slower.** In one run, the frame rate dropped to about 16 fps for about a minute after a
  level loaded. It was not investigated.
- **Debug windows that open a second window can crash the game,** such as the Performance Graph's Export. The PS5
  can show only the game's own window.
- **While the game runs, it does not notice files that change** in subfolders of the folders it watches. Restart it
  after copying mods in.
- **BeamNG's crash reporter is off** (`crashrpt=d`), so BeamNG cannot report its own crashes.
- **Chromium runs in single-process mode and draws in software.** Chromium does not officially support
  single-process mode. Everything used so far works, but not every menu was tried.
- **The shim still measures as it runs.** It writes a few log lines every 10 seconds and runs a helper thread that
  wakes every 2 ms to watch the clocks, plus the stall watchdog. The cost is small.

## Building

You need `clang`, `lld-link` and `llvm-dlltool` (LLVM). No Windows SDK or MinGW is needed.

```sh
tools/beamng-cef-shim/build.sh tools/beamng-cef-shim/exports-cef-150.0.20.txt out/
```

This writes `out/libcef.dll`. For another BeamNG version:

1. On a Windows PC with that version installed, list its `libcef.dll`'s exports:
   `powershell -ExecutionPolicy Bypass -File dump-exports.ps1`.
2. Build with that list instead.

[test/run.sh](test/run.sh) checks the menu copy and the scaling on real Vulkan under Wine (lavapipe with the
validation layer).

The source files:

| File | What it holds |
| --- | --- |
| `shim.c` | The CEF wrapper, switches, settings, crash logging, the thread stacks, options. |
| `vk_hook.c` | The `winevulkan.dll` hook, swapchain fixes and frame pacing. |
| `overlay.c` | The menu copy and the scaling. |
| `paint_hook.c` | CEF's paint callbacks and frame checks. |
| `watch_hook.c` | The folder-watch fix. |
| `time_hook.c` | The sleep and clock measurements. |
| `hitch.c` | The stall watchdog. |
| `sampler.c` | The optional sampler. |

## Credits

Console testing, logs and videos: Akinoryu. Code and analysis written with Claude Code.

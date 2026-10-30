# CEF in-process shim (experimental)

BeamNG.drive draws its interface with CEF (Chromium). Chromium starts helper processes for the GPU,
network and storage services and for each page, and a PS5 title cannot start processes, so BeamNG
stops a second or so after CEF starts. This shim is a `libcef.dll` stand-in that forwards every export
to the real library, renamed `libcef_real.dll`, and appends switches that ask Chromium to do that work
inside the game's process (`--single-process --in-process-gpu --no-sandbox` by default).

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
   to replace the default switches.

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
`shim:vk-size=3840x2160` picks another full size.

`test/run.sh` checks the overlay and the scaling on real Vulkan (Mesa's lavapipe under Wine, with
the Khronos validation layer and its synchronization checks).

The shim logs each step to stderr as `libcef-shim: ...` lines in the title log, and to
`libcef_shim.log` beside the DLL (Playden keeps only the start and end of a long title log). To undo, delete the
shim and rename `libcef_real.dll` back. A Steam update or file check puts the original back.

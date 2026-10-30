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

The shim logs each step to stderr as `libcef-shim: ...` lines in the title log. To undo, delete the
shim and rename `libcef_real.dll` back. A Steam update or file check puts the original back.

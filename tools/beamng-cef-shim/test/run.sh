#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Runs overlay_test.c under Wine on Mesa's lavapipe with the Khronos validation layer, on an Xvfb
# display, once per mode (small, full, bars; see overlay_test.c), and checks screenshots of the
# display after the game's own frames and after the menu. Needs wine, Xvfb, ImageMagick,
# mesa-vulkan-drivers, vulkan-validationlayers, clang/lld-link/llvm-dlltool, and Vulkan-Headers'
# include folder:
#
#   tools/beamng-cef-shim/test/run.sh /path/to/Vulkan-Headers/include
set -eu

headers=${1:?usage: run.sh VULKAN_HEADERS_INCLUDE_DIR}
here=$(cd "$(dirname "$0")" && pwd)
out=$(mktemp -d)
cd "$out"
printf 'LIBRARY kernel32.dll\nEXPORTS\n VirtualAlloc\n Sleep\n GetProcAddress\n LoadLibraryA\n GetModuleHandleA\n GetStdHandle\n WriteFile\n ExitProcess\n GetEnvironmentVariableA\n' >k32.def
printf 'LIBRARY user32.dll\nEXPORTS\n RegisterClassW\n CreateWindowExW\n DefWindowProcW\n ShowWindow\n PeekMessageW\n DispatchMessageW\n' >u32.def
llvm-dlltool -m i386:x86-64 -d k32.def -l k32.lib
llvm-dlltool -m i386:x86-64 -d u32.def -l u32.lib
clang --target=x86_64-pc-windows-msvc -O1 -ffreestanding -fno-builtin -Wno-incompatible-pointer-types -I"$headers" \
  -c "$here/overlay_test.c" -o overlay_test.obj
lld-link /nodefaultlib /entry:mainCRTStartup /subsystem:console /out:overlay_test.exe overlay_test.obj k32.lib u32.lib

Xvfb :97 -screen 0 1920x1080x24 >/dev/null 2>&1 &
xvfb=$!
trap 'kill $xvfb 2>/dev/null || true; rm -rf "$out"' EXIT
export DISPLAY=:97 WINEDEBUG=-all VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json
export VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation
export VK_LAYER_ENABLES=VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT

# The colour of the screenshot's pixel at x,y: red, green, blue, yellow, magenta, black or its hex.
pixel() {
  value=$(convert "$1" -crop "1x1+$2+$3" txt:- | tail -1 | grep -o '#[0-9A-F]\{6\}')
  case "$value" in
    '#FF0000') echo red ;; '#00FF00') echo green ;; '#0000FF') echo blue ;; '#FFFF00') echo yellow ;;
    '#FF00FF') echo magenta ;; '#000000') echo black ;; *) echo "$value" ;;
  esac
}

# wait_for TEXT: until run.log has it, or the test failed.
wait_for() {
  for _ in $(seq 1 120); do grep -q "$1\|FAILED" run.log 2>/dev/null && return 0; sleep 1; done
}

failed=0
# check MODE SHOT X Y EXPECTED...
check() {
  mode=$1 shot=$2 x=$3 y=$4
  shift 4
  got=$(pixel "$shot" "$x" "$y")
  if [ "$got" = "$1" ]; then echo "$mode: $shot at $x,$y is $got"; else echo "$mode: $shot at $x,$y is $got, expected $1"; failed=1; fi
}

for mode in small full bars; do
  rm -f run.log
  OV_TEST_MODE=$mode wine overlay_test.exe >run.log 2>&1 &
  test=$!
  wait_for 'phase 1 done'
  sleep 1
  import -window root game.png
  wait_for 'test: done'
  sleep 1
  import -window root menu.png
  wait $test || true
  cat run.log
  errors=$(grep -ciE 'Validation Error|SYNC-HAZARD' run.log || true)
  echo "$mode: validation errors: $errors"
  [ "$errors" = 0 ] || failed=1
  ! grep -q FAILED run.log || failed=1
  case $mode in
    small)
      check small game.png 320 360 blue; check small game.png 960 360 blue
      check small menu.png 320 360 red; check small menu.png 960 360 green ;;
    full)
      check full game.png 480 540 blue; check full game.png 1440 540 yellow; check full game.png 1910 1070 yellow
      check full menu.png 480 540 red; check full menu.png 1440 540 green; check full menu.png 1910 1070 green ;;
    bars)
      check bars game.png 100 540 black; check bars game.png 600 540 blue; check bars game.png 1300 540 yellow
      check bars game.png 1800 540 black
      check bars menu.png 100 540 black; check bars menu.png 600 540 red; check bars menu.png 1300 540 green
      check bars menu.png 1800 540 black ;;
  esac
done
[ "$failed" = 0 ] && echo PASS && exit 0
echo FAIL
exit 1

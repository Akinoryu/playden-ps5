#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Runs overlay_test.c under Wine on Mesa's lavapipe with the Khronos validation layer, on an Xvfb
# display, and checks that the screen shows the overlay's red/green frame instead of the blue one
# the test "game" draws. Needs wine, Xvfb, ImageMagick, mesa-vulkan-drivers,
# vulkan-validationlayers, clang/lld-link/llvm-dlltool, and Vulkan-Headers' include folder:
#
#   tools/beamng-cef-shim/test/run.sh /path/to/Vulkan-Headers/include
set -eu

headers=${1:?usage: run.sh VULKAN_HEADERS_INCLUDE_DIR}
here=$(cd "$(dirname "$0")" && pwd)
out=$(mktemp -d)
cd "$out"
printf 'LIBRARY kernel32.dll\nEXPORTS\n VirtualAlloc\n Sleep\n GetProcAddress\n LoadLibraryA\n GetModuleHandleA\n GetStdHandle\n WriteFile\n ExitProcess\n' >k32.def
printf 'LIBRARY user32.dll\nEXPORTS\n RegisterClassW\n CreateWindowExW\n DefWindowProcW\n ShowWindow\n PeekMessageW\n DispatchMessageW\n' >u32.def
llvm-dlltool -m i386:x86-64 -d k32.def -l k32.lib
llvm-dlltool -m i386:x86-64 -d u32.def -l u32.lib
clang --target=x86_64-pc-windows-msvc -O1 -ffreestanding -fno-builtin -Wno-incompatible-pointer-types -I"$headers" \
  -c "$here/overlay_test.c" -o overlay_test.obj
lld-link /nodefaultlib /entry:mainCRTStartup /subsystem:console /out:overlay_test.exe overlay_test.obj k32.lib u32.lib

Xvfb :97 -screen 0 1280x720x24 >/dev/null 2>&1 &
xvfb=$!
trap 'kill $xvfb 2>/dev/null; rm -rf "$out"' EXIT
export DISPLAY=:97 WINEDEBUG=-all VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json
export VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation
export VK_LAYER_ENABLES=VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT
wine overlay_test.exe >run.log 2>&1 &
for _ in $(seq 1 90); do grep -q 'holding the window\|FAILED' run.log 2>/dev/null && break; sleep 1; done
import -window root shot.png
left=$(convert shot.png -crop 1x1+320+360 txt:- | tail -1)
right=$(convert shot.png -crop 1x1+960+360 txt:- | tail -1)
errors=$(grep -ciE 'Validation Error|SYNC-HAZARD' run.log || true)
cat run.log
echo "left: $left"
echo "right: $right"
echo "validation errors: $errors"
case "$left$right" in *FF0000*00FF00*) [ "$errors" = 0 ] && echo PASS && exit 0 ;; esac
echo FAIL
exit 1

#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Build the libcef.dll stand-in. exports.txt lists the real libcef.dll's export
# names, one per line (dump-exports.ps1 writes it on a Windows PC). Needs clang,
# lld-link and llvm-dlltool; no Windows SDK or MinGW.
#
#   tools/beamng-cef-shim/build.sh exports.txt out/
set -eu

exports=${1:?usage: build.sh exports.txt out-dir}
out=${2:?usage: build.sh exports.txt out-dir}
here=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$out"

printf 'LIBRARY kernel32.dll\nEXPORTS\n' >"$out/kernel32.def"
for f in LoadLibraryW GetProcAddress GetModuleFileNameW GetStdHandle WriteFile ReadFile CreateFileW \
    CloseHandle VirtualProtect VirtualQuery AddVectoredExceptionHandler GetModuleHandleExW GetCurrentThreadId \
    RtlLookupFunctionEntry RtlVirtualUnwind GetCurrentProcessId CreateThread Sleep CreateToolhelp32Snapshot \
    Thread32First Thread32Next OpenThread SuspendThread ResumeThread GetThreadContext GetModuleHandleW \
    GetTickCount64 FlushInstructionCache Module32FirstW Module32NextW VirtualAlloc; do
  printf '  %s\n' "$f" >>"$out/kernel32.def"
done
llvm-dlltool -m i386:x86-64 -d "$out/kernel32.def" -l "$out/kernel32.lib"

# cef_initialize is the shim's own; everything else goes to libcef_real.dll.
{
  printf 'LIBRARY libcef.dll\nEXPORTS\n  cef_initialize\n'
  tr -d '\r' <"$exports" | grep -E '^[A-Za-z_][A-Za-z0-9_@?$]*$' | grep -vx cef_initialize | sort -u |
    while read -r name; do printf '  %s=libcef_real.%s\n' "$name" "$name"; done
} >"$out/libcef.def"
echo "forwarding $(($(wc -l <"$out/libcef.def") - 3)) exports"

clang --target=x86_64-pc-windows-msvc -O2 -ffreestanding -fno-builtin -fno-stack-protector -Wall -Wextra \
  -c "$here/shim.c" -o "$out/shim.obj"
lld-link /dll /nodefaultlib /entry:DllMain /machine:x64 /def:"$out/libcef.def" /out:"$out/libcef.dll" \
  "$out/shim.obj" "$out/kernel32.lib"
echo "built $out/libcef.dll"

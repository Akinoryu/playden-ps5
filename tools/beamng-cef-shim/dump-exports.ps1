# SPDX-License-Identifier: GPL-3.0-or-later
#
# Write the export names of a game's libcef.dll to libcef-exports.txt in the
# current folder, with its file version on the first line as a comment.
#
#   powershell -ExecutionPolicy Bypass -File dump-exports.ps1 "C:\...\Bin64\libcef.dll"
param([string]$Path = "C:\Program Files (x86)\Steam\steamapps\common\BeamNG.drive\Bin64\libcef.dll")

$fs = [IO.File]::OpenRead($Path)
$br = New-Object IO.BinaryReader($fs)
function U16($o) { $fs.Position = $o; return $br.ReadUInt16() }
function U32($o) { $fs.Position = $o; return [int64]$br.ReadUInt32() }

$pe = U32 0x3c
$sections = U16 ($pe + 6)
$optional = $pe + 24
$first_section = $optional + (U16 ($pe + 20))
if ((U16 $optional) -ne 0x20b) { throw "not a 64-bit DLL" }
$export_rva = U32 ($optional + 112)

function Offset($rva) {
  for ($i = 0; $i -lt $sections; $i++) {
    $s = $first_section + 40 * $i
    $va = U32 ($s + 12); $size = [Math]::Max((U32 ($s + 8)), (U32 ($s + 16))); $raw = U32 ($s + 20)
    if ($rva -ge $va -and $rva -lt $va + $size) { return $rva - $va + $raw }
  }
  throw "RVA $rva is in no section"
}

$dir = Offset $export_rva
$count = U32 ($dir + 24)
$names = Offset (U32 ($dir + 32))
$lines = New-Object Collections.Generic.List[string]
$lines.Add("# " + (Get-Item $Path).VersionInfo.FileVersion)
for ($i = 0; $i -lt $count; $i++) {
  $fs.Position = Offset (U32 ($names + 4 * $i))
  $sb = New-Object Text.StringBuilder
  while (($c = $br.ReadByte()) -ne 0) { [void]$sb.Append([char]$c) }
  $lines.Add($sb.ToString())
}
$fs.Close()
[IO.File]::WriteAllLines((Join-Path (Get-Location) "libcef-exports.txt"), $lines)
"wrote $count export names to libcef-exports.txt"

# Packs resources\icons\qode-<size>.png (16 ... 256) into resources\icons\qode.ico, the icon of qode.exe
# (RC_ICONS in QODE.pro). The entries stay PNG-compressed, which Windows Vista and newer read.
# Run again whenever the PNGs change:  powershell -ExecutionPolicy Bypass -File scripts\win\make-ico.ps1

$ErrorActionPreference = 'Stop'
$root  = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$icons = Join-Path $root 'resources\icons'
$sizes = 16, 24, 32, 48, 64, 128, 256

$entries = @()
foreach ($s in $sizes) {
    $png = Join-Path $icons "qode-$s.png"
    if (Test-Path $png) { $entries += [pscustomobject]@{ Size = $s; Bytes = [IO.File]::ReadAllBytes($png) } }
}
if ($entries.Count -eq 0) { throw "No qode-<size>.png found in $icons" }

$ms = New-Object IO.MemoryStream
$w  = New-Object IO.BinaryWriter($ms)
$w.Write([UInt16]0)                  # reserved
$w.Write([UInt16]1)                  # type: icon
$w.Write([UInt16]$entries.Count)
$offset = 6 + 16 * $entries.Count
foreach ($e in $entries) {
    $dim = if ($e.Size -ge 256) { 0 } else { $e.Size }   # 0 means 256
    $w.Write([byte]$dim); $w.Write([byte]$dim)
    $w.Write([byte]0)                # palette colours
    $w.Write([byte]0)                # reserved
    $w.Write([UInt16]1)              # planes
    $w.Write([UInt16]32)             # bits per pixel
    $w.Write([UInt32]$e.Bytes.Length)
    $w.Write([UInt32]$offset)
    $offset += $e.Bytes.Length
}
foreach ($e in $entries) { $w.Write($e.Bytes) }
$w.Flush()
$out = Join-Path $icons 'qode.ico'
[IO.File]::WriteAllBytes($out, $ms.ToArray())
"Wrote $out ($($entries.Count) sizes: $(($entries | ForEach-Object Size) -join ', '))"

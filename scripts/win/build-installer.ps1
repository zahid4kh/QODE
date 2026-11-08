<#
  QODE Windows packages: the installer and the portable zip, for one flavour.

    powershell -ExecutionPolicy Bypass -File scripts\win\build-installer.ps1 [-NoWeb] [-NoBuild] [-NoPortable]

    -NoWeb       without Qt WebEngine (build-win-noweb\, files named ...-noweb-...)
    -NoBuild     use the existing build (skip scripts\win\compile.bat)
    -NoPortable  only the installer

  Steps:
    1. scripts\win\compile.bat [noweb]                    -> build-win[-noweb]\qode.exe
    2. staging-win\<web|noweb>\ (this script's own folder, emptied first): qode.exe, windeployqt's Qt DLLs and
       plugins, and the MSVC runtime DLLs next to it (app-local, so no vc_redist and no admin rights are needed)
    3. Inno Setup 6 (ISCC; %QODE_ISCC% or the usual install folders): packaging\windows\qode.iss
                                                             -> dist-win\QODE-<version>[-noweb]-setup.exe
    4. the same files plus portable.txt, as a top folder QODE\ -> dist-win\QODE-<version>[-noweb]-portable.zip
#>
param([switch]$NoWeb, [switch]$NoBuild, [switch]$NoPortable)
$ErrorActionPreference = 'Stop'

$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$flavor = if ($NoWeb) { 'noweb' } else { 'web' }
$suffix = if ($NoWeb) { '-noweb' } else { '' }
$build = if ($NoWeb) { Join-Path $root 'build-win-noweb' } else { Join-Path $root 'build-win' }
$stagingRoot = Join-Path $root 'staging-win'
$stage = Join-Path $stagingRoot $flavor
$dist = Join-Path $root 'dist-win'
$utf8Bom = New-Object System.Text.UTF8Encoding($true)

function Step($text) { Write-Host ''; Write-Host "  == $text" -ForegroundColor Cyan }
function Fail($text) { Write-Host "  $text" -ForegroundColor Red; exit 1 }

$m = Select-String -Path (Join-Path $root 'QODE.pro') -Pattern '^VERSION\s*=\s*(\S+)'
if (-not $m) { Fail 'VERSION not found in QODE.pro' }
$version = $m.Matches[0].Groups[1].Value
Write-Host "  QODE $version ($flavor)"

# --- 1. build ---------------------------------------------------------------------------------------------------
if (-not $NoBuild) {
    Step 'Building (scripts\win\compile.bat)'
    $arg = if ($NoWeb) { 'noweb' } else { '' }
    & cmd /c "`"$root\scripts\win\compile.bat`" $arg"
    if ($LASTEXITCODE -ne 0) { Fail 'The build failed.' }
}
$exe = @("$build\qode.exe", "$build\release\qode.exe") | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $exe) { Fail "No qode.exe in $build (build first, or leave out -NoBuild)." }
$qtbinFile = Join-Path $build '.qode-qtbin'
if (-not (Test-Path $qtbinFile)) { Fail "$qtbinFile is missing: build with scripts\win\compile.bat first." }
$qtbin = (Get-Content $qtbinFile -TotalCount 1).Trim()

# --- 2. stage ---------------------------------------------------------------------------------------------------
Step "Staging into $stage"
if (Test-Path $stage) {
    # Only ever this script's own output folder.
    if (-not ((Resolve-Path $stage).Path.StartsWith($stagingRoot + '\'))) { Fail "Refusing to empty $stage" }
    Remove-Item -LiteralPath $stage -Recurse -Force
}
New-Item -ItemType Directory -Force $stage | Out-Null
Copy-Item $exe $stage
& "$qtbin\windeployqt.exe" --release --no-translations --no-compiler-runtime --verbose 0 (Join-Path $stage 'qode.exe')
if ($LASTEXITCODE -ne 0) { Fail 'windeployqt failed.' }
$web = @(Get-ChildItem $stage -Filter 'Qt6WebEngine*.dll')
if ($NoWeb -and $web.Count -gt 0) { Fail 'The noweb build pulled in Qt WebEngine: check CONFIG+=no_webengine.' }
if (-not $NoWeb -and -not (Test-Path (Join-Path $stage 'QtWebEngineProcess.exe'))) { Fail 'QtWebEngineProcess.exe is missing from the web build.' }

# The MSVC runtime, app-local: Microsoft's redistributable DLLs from the Visual Studio that built QODE.
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vs = & $vswhere -latest -products * -property installationPath
$crt = Get-ChildItem (Join-Path $vs 'VC\Redist\MSVC') -Directory |
    Where-Object { $_.Name -match '^\d' } | Sort-Object { [version]$_.Name } -Descending |
    ForEach-Object { Get-ChildItem (Join-Path $_.FullName 'x64') -Directory -Filter 'Microsoft.VC*.CRT' -ErrorAction SilentlyContinue } |
    Select-Object -First 1
if (-not $crt) { Fail "No MSVC runtime (VC\Redist\MSVC\<version>\x64\Microsoft.VC*.CRT) under $vs" }
Copy-Item (Join-Path $crt.FullName '*.dll') $stage
Write-Host "    MSVC runtime from $($crt.FullName)"

# Direct3D's shader compiler, which windeployqt finds only inside a Visual Studio environment (compile.bat has one):
# the Windows SDK's redistributable copy, as in the dev build.
$kits = (Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows Kits\Installed Roots' -ErrorAction SilentlyContinue).KitsRoot10
if (-not $kits) { $kits = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10' }
$d3d = Join-Path $kits 'Redist\D3D\x64'
foreach ($dll in 'dxcompiler.dll', 'dxil.dll') {
    if (Test-Path (Join-Path $d3d $dll)) { Copy-Item (Join-Path $d3d $dll) $stage } else { Write-Host "    note: $dll not found in $d3d" }
}

# --- 3. installer -----------------------------------------------------------------------------------------------
Step 'Installer (Inno Setup)'
$iscc = @($env:QODE_ISCC, "$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe", "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe",
          "$env:ProgramFiles\Inno Setup 6\ISCC.exe") | Where-Object { $_ -and (Test-Path $_) } | Select-Object -First 1
if (-not $iscc) { Fail 'Inno Setup 6 not found: winget install --id JRSoftware.InnoSetup -e --scope user (or set QODE_ISCC).' }
New-Item -ItemType Directory -Force $dist | Out-Null

# "What's new" page after installing: this version's changelog notes (the .deb postinst shows the same).
$notes = @()
$inEntry = $false
foreach ($line in Get-Content (Join-Path $root 'packaging\changelog') -Encoding UTF8) {
    if ($line -match '^qode \(') { if ($inEntry) { break }; $inEntry = $true; continue }
    if ($inEntry -and $line -match '^\s+\*\s+(.*)$') { $notes += "  - $($Matches[1])" }
}
$flavorText = if ($NoWeb) { 'without the web preview (pages open in your browser; the other installer has it)' } else { 'with the web preview (Qt WebEngine)' }
$whatsNew = Join-Path $stagingRoot "whatsnew-$flavor.txt"
$text = @(
    "QODE $version is installed, $flavorText.",
    '',
    'In this version:'
) + $notes + @(
    '',
    'Where QODE keeps your data (nowhere else):',
    '  %APPDATA%\QODE-AGENTIC        settings, API keys, per-project state',
    '  %LOCALAPPDATA%\QODE-AGENTIC   language servers it downloads, local history, caches',
    '',
    "Help > QODE's Files in QODE lists everything it created, with sizes.",
    'Language servers are never bundled: QODE offers to download them when a project needs one.',
    'To remove QODE: Settings > Apps > QODE > Uninstall. It also offers to delete the folders above.'
)
[IO.File]::WriteAllText($whatsNew, ($text -join "`r`n") + "`r`n", $utf8Bom)

& $iscc /Q "/DAppVersion=$version" "/DFlavor=$flavor" "/DSourceDir=$stage" "/DOutputDir=$dist" "/DWhatsNew=$whatsNew" (Join-Path $root 'packaging\windows\qode.iss')
if ($LASTEXITCODE -ne 0) { Fail 'Inno Setup failed.' }
$setup = Join-Path $dist "QODE-$version$suffix-setup.exe"

# --- 4. portable zip --------------------------------------------------------------------------------------------
$zip = $null
if (-not $NoPortable) {
    Step 'Portable zip'
    # A copy of the staged program as QODE\, plus portable.txt (the installer's files never contain it).
    $extra = Join-Path $stagingRoot "portable-$flavor"
    if (Test-Path $extra) {
        if (-not ((Resolve-Path $extra).Path.StartsWith($stagingRoot + '\'))) { Fail "Refusing to empty $extra" }
        Remove-Item -LiteralPath $extra -Recurse -Force
    }
    New-Item -ItemType Directory -Force $extra | Out-Null
    Copy-Item -LiteralPath $stage -Destination (Join-Path $extra 'QODE') -Recurse
    $marker = @(
        'This file makes QODE portable. While it sits next to qode.exe, QODE keeps everything it writes in the',
        'data folder here: settings and API keys (data\config), language servers, local history and caches',
        '(data\local), JDKs it downloads (data\local\jdks). Nothing is written to your user profile or the registry.',
        'Delete this whole folder to remove QODE completely. Delete only this file to make QODE use the normal folders',
        '(%APPDATA%\QODE-AGENTIC, %LOCALAPPDATA%\QODE-AGENTIC) instead.'
    )
    [IO.File]::WriteAllText((Join-Path $extra 'QODE\portable.txt'), ($marker -join "`r`n") + "`r`n", (New-Object System.Text.UTF8Encoding($false)))
    $zip = Join-Path $dist "QODE-$version$suffix-portable.zip"
    if (Test-Path $zip) { Remove-Item -LiteralPath $zip -Force }
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    [IO.Compression.ZipFile]::CreateFromDirectory((Join-Path $extra 'QODE'), $zip, [IO.Compression.CompressionLevel]::Optimal, $true)
}

Step 'Done'
foreach ($f in @($setup, $zip) | Where-Object { $_ }) {
    $item = Get-Item $f
    $hash = (Get-FileHash $f -Algorithm SHA256).Hash.ToLower()
    Write-Host ("    {0}  {1:N0} MB" -f $item.FullName, ($item.Length / 1MB))
    Write-Host "      sha256 $hash"
}

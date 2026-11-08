<#
  Both Windows flavours (with and without Qt WebEngine): installers and portable zips into dist-win\.

    powershell -ExecutionPolicy Bypass -File scripts\win\build-all.ps1 [-NoBuild]
#>
param([switch]$NoBuild)
$ErrorActionPreference = 'Stop'
$script = Join-Path $PSScriptRoot 'build-installer.ps1'
& $script -NoBuild:$NoBuild
if ($LASTEXITCODE) { exit $LASTEXITCODE }
& $script -NoWeb -NoBuild:$NoBuild
if ($LASTEXITCODE) { exit $LASTEXITCODE }

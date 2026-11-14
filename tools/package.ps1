# Makes SharedCrew-<version>.zip (the plugin folder, ready to unzip into X-Plane's plugins folder).
# Usage: powershell -ExecutionPolicy Bypass -File tools\package.ps1 [-OutDir <folder>]
param(
    [string]$OutDir = (Join-Path $HOME "Documents")
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$src = Join-Path $root "build\dist\SharedCrew"
if (-not (Test-Path (Join-Path $src "win_x64\SharedCrew.xpl"))) { throw "Build the plugin first (see README)." }
$cmake = Get-Content (Join-Path $root "CMakeLists.txt") -Raw
if ($cmake -notmatch 'project\(SharedCrew VERSION ([0-9.]+)') { throw "Version not found in CMakeLists.txt" }
$zip = Join-Path $OutDir ("SharedCrew-" + $Matches[1] + ".zip")
if (Test-Path $zip) { Remove-Item $zip }
Compress-Archive -Path $src -DestinationPath $zip
Write-Host "Created $zip"

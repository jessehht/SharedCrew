# Copies the built plugin into X-Plane's plugins folder.
# Usage: powershell -ExecutionPolicy Bypass -File tools\install.ps1 [-XPlane "C:\X-Plane 12"]
param(
    [string]$XPlane = "C:\Program Files (x86)\Steam\steamapps\common\X-Plane 12"
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$src = Join-Path $root "build\dist\SharedCrew"
$dst = Join-Path $XPlane "Resources\plugins\SharedCrew"

if (-not (Test-Path (Join-Path $src "win_x64\SharedCrew.xpl"))) { throw "Build the plugin first (see README)." }
if (-not (Test-Path (Join-Path $XPlane "Resources\plugins"))) { throw "X-Plane not found in $XPlane" }
if (Get-Process -Name "X-Plane" -ErrorAction SilentlyContinue) { throw "Close X-Plane first." }

New-Item -ItemType Directory -Force (Join-Path $dst "win_x64") | Out-Null
Copy-Item (Join-Path $src "win_x64\SharedCrew.xpl") (Join-Path $dst "win_x64") -Force
Copy-Item (Join-Path $src "profiles") $dst -Recurse -Force
Write-Host "Installed to $dst"

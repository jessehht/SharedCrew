# Downloads the official X-Plane SDK into third_party/XPSDK.
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$dir = Join-Path $root "third_party"
$zip = Join-Path $dir "XPSDK430.zip"
New-Item -ItemType Directory -Force $dir | Out-Null
Invoke-WebRequest -UseBasicParsing "https://developer.x-plane.com/wp-content/plugins/code-sample-generation/sdk_zip_files/XPSDK430.zip" -OutFile $zip
$tmp = Join-Path $dir "xpsdk_extract"
Expand-Archive -Force $zip $tmp
if (Test-Path (Join-Path $dir "XPSDK")) { Remove-Item -Recurse -Force (Join-Path $dir "XPSDK") }
Move-Item (Join-Path $tmp "SDK") (Join-Path $dir "XPSDK")
Remove-Item -Recurse -Force $tmp, $zip
Write-Host "SDK ready in $dir\XPSDK"

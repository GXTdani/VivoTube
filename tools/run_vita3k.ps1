# Copies a fresh build into an existing Vita3K install of VivoTube and launches it.
# Install the VPK once first (File > Install .vpk, or run Vita3K.exe with the .vpk path).
#   powershell -ExecutionPolicy Bypass -File tools\run_vita3k.ps1 [-Vita3K C:\path\to\Vita3K.exe]
param(
    [string]$Vita3K = "$PSScriptRoot\..\..\tools\Vita3K\Vita3K.exe",
    [string]$TitleId = "VIVO00001"
)
$ErrorActionPreference = "Stop"
$root = Resolve-Path "$PSScriptRoot\.."
$prefLine = Select-String -Path (Join-Path (Split-Path $Vita3K) "config.yml") -Pattern '^pref-path:' | Select-Object -First 1
$pref = if ($prefLine) { ($prefLine.Line -replace '^pref-path:\s*', '').Trim('"', ' ') } else { "" }
if (-not $pref) { $pref = Join-Path $env:APPDATA "Vita3K\Vita3K" }
$app = Join-Path $pref "ux0\app\$TitleId"
if (-not (Test-Path $app)) { throw "VivoTube is not installed in Vita3K yet ($app). Install the VPK once first." }

Get-Process Vita3K -ErrorAction SilentlyContinue | ForEach-Object { $_.CloseMainWindow() | Out-Null }
Start-Sleep -Seconds 2
Get-Process Vita3K -ErrorAction SilentlyContinue | Stop-Process -Force

Copy-Item (Join-Path $root "build\eboot.bin") (Join-Path $app "eboot.bin") -Force
Copy-Item (Join-Path $root "assets\*") (Join-Path $app "assets") -Force -Recurse
Write-Output "Deployed to $app"
Start-Process -FilePath $Vita3K -ArgumentList "-r", $TitleId -WorkingDirectory (Split-Path $Vita3K)

# Development loop: installs the freshly built plugin and restarts New Vegas through xNVSE.
# Changes to the core alone don't need this: Deploy-Core.ps1 reloads it in the running game.
param([string]$GameDir = 'C:\games\Steam\steamapps\common\Fallout New Vegas')
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
Stop-Process -Name FalloutNV -Force -ErrorAction SilentlyContinue
Start-Sleep -Seconds 3
$plugins = Join-Path $GameDir 'Data/NVSE/Plugins'
Copy-Item (Join-Path $projectRoot 'build/native/VegasCraft.dll') (Join-Path $plugins 'VegasCraft.dll') -Force
New-Item -ItemType Directory (Join-Path $plugins 'VegasCraft') -Force | Out-Null
Copy-Item (Join-Path $projectRoot 'build/native/VegasCraftCore.dll') (Join-Path $plugins 'VegasCraft/VegasCraftCore.dll') -Force
# The FNV 4GB Patcher (it leaves FalloutNV_backup.exe) makes FalloutNV.exe load xNVSE itself.
$launcher = if (Test-Path (Join-Path $GameDir 'FalloutNV_backup.exe')) { 'FalloutNV.exe' } else { 'nvse_loader.exe' }
Start-Process -FilePath (Join-Path $GameDir $launcher) -WorkingDirectory $GameDir
Write-Host 'New Vegas restarted with the new plugin'

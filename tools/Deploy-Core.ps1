# Development loop without restarting New Vegas: builds the native plugin, installs its core, and
# waits for the running game to reload it (VegasCraft.log: "core: reloaded"). The loader
# (VegasCraft.dll) can only be replaced with New Vegas closed: use Restart-NewVegas.ps1 for that.
param([string]$GameDir = 'C:\games\Steam\steamapps\common\Fallout New Vegas', [switch]$SkipTests)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$build = Join-Path $projectRoot 'build/native'
& cmake --build $build
if ($LASTEXITCODE) { throw 'Native build failed' }
if (!$SkipTests) {
    & ctest --test-dir $build --output-on-failure
    if ($LASTEXITCODE) { throw 'Native tests failed' }
}
$plugins = Join-Path $GameDir 'Data/NVSE/Plugins'
$coreTarget = Join-Path $plugins 'VegasCraft/VegasCraftCore.dll'
$loaderTarget = Join-Path $plugins 'VegasCraft.dll'
$running = [bool](Get-Process FalloutNV -ErrorAction SilentlyContinue)
$loaderChanged = !(Test-Path $loaderTarget) -or (Get-FileHash (Join-Path $build 'VegasCraft.dll')).Hash -ne (Get-FileHash $loaderTarget).Hash
if ($loaderChanged) {
    if ($running) { Write-Warning 'VegasCraft.dll (the loader) changed: New Vegas must be restarted for that part (Restart-NewVegas.ps1)' }
    else { Copy-Item (Join-Path $build 'VegasCraft.dll') $loaderTarget -Force }
}
New-Item -ItemType Directory (Split-Path $coreTarget) -Force | Out-Null
$log = Join-Path $GameDir 'VegasCraft.log'
$before = if (Test-Path $log) { (Get-Item $log).Length } else { 0 }
Copy-Item (Join-Path $build 'VegasCraftCore.dll') $coreTarget -Force
if (!$running) { Write-Host 'Core installed; New Vegas is not running'; return }
$deadline = (Get-Date).AddSeconds(20)
while ((Get-Date) -lt $deadline) {
    Start-Sleep -Milliseconds 250
    $stream = [System.IO.File]::Open($log, 'Open', 'Read', 'ReadWrite')
    try {
        if ($stream.Length -le $before) { continue }
        [void]$stream.Seek($before, 'Begin')
        $text = (New-Object System.IO.StreamReader($stream)).ReadToEnd()
    } finally { $stream.Dispose() }
    $done = $text -split "`n" | Where-Object { $_ -match 'core: (reloaded|no core running|the core refused)' } | Select-Object -Last 1
    if ($done) {
        Write-Host $done.Trim()
        if ($done -notmatch 'reloaded') { exit 1 }
        return
    }
}
Write-Warning 'New Vegas did not reload the core within 20 s (still loading, or hot reload off?)'
exit 1

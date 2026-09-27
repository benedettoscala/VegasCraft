param([string]$CompilerRoot)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$buildRoot = Join-Path $projectRoot 'build/native'
if (!$CompilerRoot) {
    $CompilerRoot = Get-ChildItem (Join-Path $projectRoot '.tools') -Directory -Filter 'llvm-mingw-*-ucrt-x86_64' |
        Sort-Object Name -Descending | Select-Object -First 1 -ExpandProperty FullName
}
if (!$CompilerRoot) { throw 'Pass -CompilerRoot with the path to a Windows LLVM-MinGW toolchain, or use MSVC with CMake -A Win32.' }
$compiler = Join-Path $CompilerRoot 'bin/i686-w64-mingw32-clang++.exe'
if (!(Test-Path -LiteralPath $compiler)) { throw "Missing compiler: $compiler" }
$ninjaCommand = Get-Command ninja -ErrorAction SilentlyContinue
if ($ninjaCommand) { $ninja = $ninjaCommand.Source }
else { $ninja = Join-Path (Split-Path (Get-Command cmake).Source) 'ninja.exe' }
if (!(Test-Path -LiteralPath $ninja)) { throw 'Ninja was not found.' }
& cmake -S $projectRoot -B $buildRoot -G Ninja "-DCMAKE_CXX_COMPILER=$compiler" "-DCMAKE_MAKE_PROGRAM=$ninja" -DCMAKE_BUILD_TYPE=Release
if ($LASTEXITCODE) { throw 'Native configuration failed' }
& cmake --build $buildRoot
if ($LASTEXITCODE) { throw 'Native build failed' }
& ctest --test-dir $buildRoot --output-on-failure
if ($LASTEXITCODE) { throw 'Native tests failed' }
Write-Host "Native plugin ready: $buildRoot/VegasCraft.dll"

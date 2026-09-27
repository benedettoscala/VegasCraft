# Packs a VegasCraft release into dist\ (after tools/Build-Native.ps1 and tools/Build-Fabric.ps1):
#   VegasCraft-<version>.zip          the New Vegas mod (install with MO2 or Vortex): the xNVSE plugin,
#                                     its ini, and VegasCraft-Minecraft.zip, the Minecraft it starts
#   vegascraft-fabric-<version>.jar   the Minecraft mod on its own (for your own launcher)
#
# VegasCraft-Minecraft.zip holds a portable Prism Launcher with a ready "VegasCraft" instance
# (Minecraft 26.3, Fabric, Fabric API, e4mc, VegasCraft). The plugin unpacks it to
# %LOCALAPPDATA%\VegasCraft and starts it; Prism asks the player to sign in once, then downloads
# Minecraft and Java itself. Adapted from SkyCraft's tools/package.ps1 (MIT).
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
$root = Split-Path -Parent $PSScriptRoot
$version = (Select-String -Path "$root\fabric\gradle.properties" -Pattern '^version=(.+)$').Matches[0].Groups[1].Value.Trim()

# Pinned downloads (checked against these hashes), as in SkyCraft 0.1.2.
$prismVersion = '11.1.1'
$prismZip = "PrismLauncher-Windows-MSVC-Portable-$prismVersion.zip"
$prismUrl = "https://github.com/PrismLauncher/PrismLauncher/releases/download/$prismVersion/$prismZip"
$prismSha256 = 'ab35a770fb06d89d2ccc098079db5db329fb4e68f42b72babd8b095efde3d2d7'
$prismLicenseUrl = "https://raw.githubusercontent.com/PrismLauncher/PrismLauncher/$prismVersion/LICENSE"
$fabricApiJar = 'fabric-api-0.161.0+26.3.jar'
$fabricApiUrl = 'https://cdn.modrinth.com/data/P7dR8mSH/versions/bNnaTiuM/fabric-api-0.161.0%2B26.3.jar'
$fabricApiSha512 = 'ed6b2586d6fde11fde8472f5a527c51e99b67026e46f94d4bfd85e7e28ce5ee299173ee16ad576ceb51f39f98d30a811086a6deb1a86a524859cc16e12da109d'
$e4mcJar = 'e4mc-fabric-6.2.2-modern.jar'
$e4mcUrl = 'https://cdn.modrinth.com/data/qANg5Jrr/versions/AouleFRY/e4mc-fabric-6.2.2-modern.jar'
$e4mcSha512 = '01ef0a8c5b76e2cb0effd337bad3350d8807d100d0ec661e01b2ffb20af7b652f756c5eaa11bee233c37905bfd7b573f7a85f3d15bfd2833962c76f02cd59a86'

function Get-Pinned([string]$url, [string]$path, [string]$algorithm, [string]$hash) {
    if (-not (Test-Path $path)) {
        New-Item -ItemType Directory (Split-Path $path) -Force | Out-Null
        Invoke-WebRequest -Uri $url -OutFile $path -UseBasicParsing
    }
    if ($hash -and (Get-FileHash $path -Algorithm $algorithm).Hash -ne $hash.ToUpper()) {
        throw "$path doesn't match its pinned $algorithm hash"
    }
}
# Zip entries with forward slashes, as every mod manager expects.
Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
function New-Zip([string]$path, [System.Collections.IDictionary]$entries) {
    if (Test-Path $path) { Remove-Item $path }
    $zip = [System.IO.Compression.ZipFile]::Open($path, [System.IO.Compression.ZipArchiveMode]::Create)
    try {
        foreach ($name in $entries.Keys) {
            [System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zip, $entries[$name], $name, [System.IO.Compression.CompressionLevel]::Optimal) | Out-Null
        }
    } finally { $zip.Dispose() }
}
function New-ZipFromFolder([string]$path, [string]$folder) {
    $entries = [ordered]@{}
    $base = (Resolve-Path $folder).Path.TrimEnd('\') + '\'
    Get-ChildItem $folder -Recurse -File | Sort-Object FullName | ForEach-Object {
        $entries[$_.FullName.Substring($base.Length).Replace('\', '/')] = $_.FullName
    }
    New-Zip $path $entries
}

$dll = "$root\build\native\VegasCraft.dll"
$core = "$root\build\native\VegasCraftCore.dll"
$jar = "$root\fabric\build\libs\vegascraft-fabric-$version.jar"
foreach ($f in @($dll, $core, $jar)) {
    if (-not (Test-Path $f)) { throw "missing $f (build both halves first)" }
}
$cache = "$root\.tools\prism"
Get-Pinned $prismUrl "$cache\$prismZip" SHA256 $prismSha256
Get-Pinned $fabricApiUrl "$cache\$fabricApiJar" SHA512 $fabricApiSha512
Get-Pinned $e4mcUrl "$cache\$e4mcJar" SHA512 $e4mcSha512
Get-Pinned $prismLicenseUrl "$cache\PrismLauncher-$prismVersion-LICENSE.txt" '' ''

$dist = "$root\dist"
New-Item -ItemType Directory $dist -Force | Out-Null
$bundle = "$dist\bundle"
$bundle = [System.IO.Path]::GetFullPath($bundle)
$expectedBundle = [System.IO.Path]::GetFullPath((Join-Path $root 'dist\bundle'))
if ($bundle -ne $expectedBundle -or -not $bundle.StartsWith([System.IO.Path]::GetFullPath($dist).TrimEnd('\') + '\', [System.StringComparison]::OrdinalIgnoreCase)) {
    throw 'Bundle cleanup target must be the project dist\bundle directory'
}
if (Test-Path -LiteralPath $bundle) { Remove-Item -LiteralPath $bundle -Recurse -Force }
Copy-Item -Recurse "$root\tools\minecraft-bundle" $bundle
Expand-Archive "$cache\$prismZip" "$bundle\Prism" -Force
Copy-Item "$cache\PrismLauncher-$prismVersion-LICENSE.txt" "$bundle\Prism\LICENSE-PrismLauncher.txt"
(Get-Content "$bundle\Prism\THIRD-PARTY.txt" -Raw).Replace('{PRISM_VERSION}', $prismVersion) | Set-Content "$bundle\Prism\THIRD-PARTY.txt" -NoNewline
$mods = "$bundle\Prism\instances\VegasCraft\.minecraft\mods"
New-Item -ItemType Directory $mods -Force | Out-Null
Copy-Item "$cache\$fabricApiJar" $mods
Copy-Item "$cache\$e4mcJar" $mods
Copy-Item $jar "$mods\vegascraft-$version.jar"
Set-Content "$bundle\bundle-version.txt" "VegasCraft $version, Prism Launcher $prismVersion, $fabricApiJar, $e4mcJar" -NoNewline
New-ZipFromFolder "$dist\VegasCraft-Minecraft.zip" $bundle
Remove-Item -LiteralPath $bundle -Recurse -Force

New-Zip "$dist\VegasCraft-$version.zip" ([ordered]@{
    'NVSE/Plugins/VegasCraft.dll' = $dll
    'NVSE/Plugins/VegasCraft.ini' = "$root\config\VegasCraft.ini"
    'NVSE/Plugins/VegasCraft/VegasCraftCore.dll' = $core
    'NVSE/Plugins/VegasCraft/VegasCraft-Minecraft.zip' = "$dist\VegasCraft-Minecraft.zip"
    'NVSE/Plugins/VegasCraft/LICENSE.txt' = "$root\LICENSE"
    'NVSE/Plugins/VegasCraft/THIRD-PARTY-NOTICES.md' = "$root\THIRD-PARTY-NOTICES.md"
    'NVSE/Plugins/VegasCraft/SkyCraft-MIT.txt' = "$root\licenses\SkyCraft-MIT.txt"
})
Copy-Item $jar "$dist\vegascraft-fabric-$version.jar" -Force
Remove-Item "$dist\VegasCraft-Minecraft.zip"
Get-ChildItem $dist -File | ForEach-Object { '{0,-40} {1,12:N0} bytes' -f $_.Name, $_.Length }

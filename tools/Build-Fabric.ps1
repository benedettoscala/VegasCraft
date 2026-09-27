param([string]$JavaHome)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
if (!$JavaHome) {
    $JavaHome = Get-ChildItem (Join-Path $projectRoot '.tools') -Directory -Filter 'jdk-25*' |
        Sort-Object Name -Descending | Select-Object -First 1 -ExpandProperty FullName
}
if (!$JavaHome) { throw 'Pass -JavaHome with a JDK 25 directory.' }
$previousJava = $env:JAVA_HOME
$previousGradle = $env:GRADLE_USER_HOME
try {
    $env:JAVA_HOME = $JavaHome
    $env:GRADLE_USER_HOME = Join-Path $projectRoot '.tools/gradle-cache'
    Push-Location (Join-Path $projectRoot 'fabric')
    try {
        & ./gradlew.bat build --no-daemon
        if ($LASTEXITCODE) { throw 'Fabric build failed' }
    } finally { Pop-Location }
} finally {
    $env:JAVA_HOME = $previousJava
    $env:GRADLE_USER_HOME = $previousGradle
}

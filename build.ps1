# Builds Tuki with the MSVC Build Tools.
#   .\build.ps1            -> build\Tuki.exe (+ ui folder)
#   .\build.ps1 -Test      -> also builds and runs the unit tests
#   .\build.ps1 -Debug     -> unoptimized build with debug info
param(
    [switch]$Test,
    [switch]$Debug,
    [string]$Tools = $(if ($env:TUKI_TOOLS) { $env:TUKI_TOOLS } else { 'C:\Tools' })
)
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$out = Join-Path $root 'build'
$obj = Join-Path $out 'obj'
New-Item -ItemType Directory -Force $out, $obj | Out-Null

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -prerelease -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw 'MSVC Build Tools not found.' }
$vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'

$wv2 = Join-Path $Tools 'webview2\pkg\build\native'
$json = Join-Path $Tools 'nlohmann'
foreach ($p in "$wv2\include\WebView2.h", "$json\json.hpp") {
    if (-not (Test-Path $p)) { throw "Missing dependency: $p" }
}

$opt = if ($Debug) { '/Od /Zi /MTd /D_DEBUG' } else { '/O2 /MT /DNDEBUG' }
$common = "/nologo /std:c++20 /EHsc /W4 /permissive- /utf-8 /DUNICODE /D_UNICODE /DNOMINMAX /DWIN32_LEAN_AND_MEAN $opt"

function Invoke-Msvc([string]$cmd) {
    cmd /c "call `"$vcvars`" >nul && $cmd"
    if ($LASTEXITCODE -ne 0) { throw "Build step failed: $cmd" }
}

if ($Test) {
    Write-Host 'Building unit tests...'
    Invoke-Msvc "cl $common /Fo`"$obj\\`" /Fe`"$out\engine_tests.exe`" `"$root\tests\engine_tests.cpp`" `"$root\src\engine.cpp`""
    & "$out\engine_tests.exe"
    if ($LASTEXITCODE -ne 0) { throw 'Unit tests failed.' }
}

Write-Host 'Compiling resources...'
if (-not (Test-Path (Join-Path $root 'assets\tuki.ico'))) { python (Join-Path $root 'tools\make_icon.py') }
Invoke-Msvc "cd /d `"$root\src`" && rc /nologo /fo `"$obj\tuki.res`" tuki.rc"

Write-Host 'Building Tuki.exe...'
$sources = @('main.cpp', 'engine.cpp', 'hook.cpp', 'config.cpp', 'system.cpp', 'window.cpp') | ForEach-Object { "`"$root\src\$_`"" }
$libs = 'user32.lib shell32.lib advapi32.lib ole32.lib gdi32.lib dwmapi.lib shlwapi.lib version.lib'
Invoke-Msvc ("cl $common /I`"$wv2\include`" /I`"$json`" /Fo`"$obj\\`" /Fe`"$out\Tuki.exe`" " + ($sources -join ' ') +
    " `"$obj\tuki.res`" /link /SUBSYSTEM:WINDOWS `"$wv2\x64\WebView2LoaderStatic.lib`" $libs")

Write-Host 'Copying UI...'
$uiOut = Join-Path $out 'ui'
if (Test-Path $uiOut) { Remove-Item $uiOut -Recurse -Force }
Copy-Item (Join-Path $root 'ui') $uiOut -Recurse

Write-Host "Done: $out\Tuki.exe"

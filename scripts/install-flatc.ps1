# Copyright (c) 2026 Golden Hour Games. All Rights Reserved.

param(
    [string]$Dest
)

$ErrorActionPreference = "Stop"

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$VendorDir = Split-Path -Parent $ScriptDir
$ProjectRoot = Split-Path -Parent (Split-Path -Parent $VendorDir)
$ToolsDir = Join-Path $ProjectRoot "Tools"
$BuildDir = Join-Path $VendorDir "out\build\x64-Release-VS"

if ($Dest) {
    $DestPath = $Dest
} else {
    $DestPath = Join-Path $ToolsDir "flatc.exe"
}

Write-Host "Vendor dir:   $VendorDir"
Write-Host "Project root: $ProjectRoot"
Write-Host "Destination:  $DestPath"

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) {
    Write-Error "vswhere.exe not found. Is Visual Studio installed?"
    exit 1
}

$vsPath = & $vswhere -latest -property installationPath
if (-not $vsPath) {
    Write-Error "Visual Studio not found"
    exit 1
}

$cmake = Join-Path $vsPath "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
if (-not (Test-Path $cmake)) {
    Write-Error "CMake not found at $cmake"
    exit 1
}

Write-Host "Using CMake:  $cmake"
Write-Host "Build dir:    $BuildDir"
Write-Host ""

Write-Host "Configuring..."
$configArgs = @(
    "-G", "Visual Studio 17 2022",
    "-A", "x64",
    "-S", $VendorDir,
    "-B", $BuildDir,
    "-DFLATBUFFERS_BUILD_TESTS=OFF",
    "-DFLATBUFFERS_BUILD_FLATC=ON",
    "-DFLATBUFFERS_BUILD_FLATLIB=OFF",
    "-DFLATBUFFERS_BUILD_FLATHASH=OFF"
)

& $cmake @configArgs
if ($LASTEXITCODE -ne 0) {
    Write-Error "CMake configure failed"
    exit 1
}

Write-Host ""
Write-Host "Building..."
& $cmake --build $BuildDir --target flatc --config Release
if ($LASTEXITCODE -ne 0) {
    Write-Error "Build failed"
    exit 1
}

$FlatcExe = Join-Path $BuildDir "Release\flatc.exe"
if (-not (Test-Path $FlatcExe)) {
    Write-Error "flatc.exe not found at $FlatcExe"
    exit 1
}

Write-Host ""
Write-Host "Build successful: $FlatcExe"

$DestDir = Split-Path -Parent $DestPath
if (-not (Test-Path $DestDir)) {
    New-Item -ItemType Directory -Path $DestDir | Out-Null
}

$FileExists = Test-Path $DestPath
if ($FileExists) {
    $srcHash = (Get-FileHash $FlatcExe -Algorithm MD5).Hash
    $dstHash = (Get-FileHash $DestPath -Algorithm MD5).Hash
    if ($srcHash -eq $dstHash) {
        Write-Host "No change detected - flatc.exe is already up to date"
        exit 0
    }
    Write-Host "Change detected, updating..."
    & p4 edit $DestPath 2>$null
}

Copy-Item -Path $FlatcExe -Destination $DestPath -Force
Write-Host "Installed to $DestPath"

if (-not $FileExists) {
    & p4 add $DestPath 2>$null
}

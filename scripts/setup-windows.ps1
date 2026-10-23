<#
.SYNOPSIS
  Windows setup: vcpkg + FFmpeg with NVIDIA NVDEC/NVENC, inside the workspace.

.DESCRIPTION
  - Everything lives under <workspace>\third_party\vcpkg. Nothing is installed system-wide.
  - Builds FFmpeg from source with MSVC (LGPL: no 'gpl' or 'nonfree' feature, no CUDA toolkit).
  - NVIDIA libraries (nvcuda.dll, nvcuvid.dll, nvEncodeAPI64.dll) are loaded at runtime from the driver.
  - Calls the vcpkg.exe inside the workspace clone by full path, so the vcpkg bundled with Visual Studio
    (the one `where vcpkg` may show) is not used.
  - Builds only libavformat, libavcodec and libavutil (plus NVIDIA support) by default, no ffmpeg.exe.
  - Safe to re-run: an existing vcpkg clone and installed packages are reused.
  - First run takes a while (vcpkg downloads MSYS2/NASM and compiles FFmpeg).

  Prerequisites (install once, then open a NEW terminal):
    winget install Git.Git
    winget install Kitware.CMake
    winget install Microsoft.VisualStudio.2022.BuildTools --override "--wait --passive --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"

.PARAMETER Workspace
  Folder that contains the parallax project. Default: the parent of the parallax folder.
  Use a short path without spaces, for example C:\Laboratory\cpp-workspace.
  CMakePresets.json expects vcpkg at <parallax>\..\third_party\vcpkg, so leave this at the default.

.PARAMETER Features
  vcpkg feature list for the ffmpeg port. 'core' turns off the default features (avdevice, avfilter,
  swscale, swresample). If configure fails with the lean default, retry with: -Features nvcodec

.PARAMETER VcpkgRef
  Optional vcpkg commit or tag to pin. Empty = latest master (shallow clone).

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File parallax\scripts\setup-windows.ps1
#>
[CmdletBinding()]
param(
    [string]$Workspace = '',
    [string]$Triplet = 'x64-windows',
    [string]$Features = 'core,avcodec,avformat,nvcodec',
    [string]$VcpkgRef = ''
)
$ErrorActionPreference = 'Stop'

# $PSScriptRoot can be empty inside param() defaults on Windows PowerShell 5.1, so resolve it here.
if (-not $Workspace) {
    $scriptDir = if ($PSScriptRoot) { $PSScriptRoot } else { Split-Path -Parent $MyInvocation.MyCommand.Path }
    $Workspace = Split-Path -Parent (Split-Path -Parent $scriptDir)
}
Write-Host "Workspace: $Workspace"

function Invoke-Checked {
    param([string]$Exe, [string[]]$Arguments)
    & $Exe @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Exe failed with exit code $LASTEXITCODE" }
}

function Require-Tool {
    param([string]$Name, [string]$Hint)
    if (-not (Get-Command $Name -ErrorAction SilentlyContinue)) {
        throw "$Name not found on PATH. $Hint (then open a new terminal)"
    }
}

Write-Host "== 1/4 prerequisites"
Require-Tool git 'Install with: winget install Git.Git'
Require-Tool cmake 'Install with: winget install Kitware.CMake'

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path $vswhere)) {
    throw 'Visual Studio / Build Tools 2022 not found. See the prerequisites in this script header.'
}
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) {
    throw 'Visual Studio is installed but has no "C++ x64 build tools" component. Add the "Desktop development with C++" workload.'
}
Write-Host "   Visual Studio: $vs"
Write-Host "   cmake:         $((cmake --version | Select-Object -First 1))"
if (Get-Command nvidia-smi -ErrorAction SilentlyContinue) {
    Write-Host "   NVIDIA driver: $((nvidia-smi --query-gpu=driver_version --format=csv,noheader | Select-Object -First 1))"
}
else {
    Write-Host '   NVIDIA driver: nvidia-smi not found (the build still works; decoding needs the driver)'
}
foreach ($dll in @('nvcuda.dll', 'nvcuvid.dll', 'nvEncodeAPI64.dll')) {
    if (Test-Path (Join-Path $env:SystemRoot "System32\$dll")) {
        Write-Host "   driver DLL ok: $dll"
    }
    else {
        Write-Host "   WARNING: $dll missing from System32 (reinstall the NVIDIA driver)"
    }
}

Write-Host "== 2/4 vcpkg"
$vcpkgDir = Join-Path $Workspace 'third_party\vcpkg'
if (-not (Test-Path $vcpkgDir)) {
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $vcpkgDir) | Out-Null
    if ($VcpkgRef) {
        Invoke-Checked git @('clone', 'https://github.com/microsoft/vcpkg.git', $vcpkgDir)
    }
    else {
        Invoke-Checked git @('clone', '--depth', '1', 'https://github.com/microsoft/vcpkg.git', $vcpkgDir)
    }
}
if ($VcpkgRef) { Invoke-Checked git @('-C', $vcpkgDir, 'checkout', $VcpkgRef) }
$vcpkg = Join-Path $vcpkgDir 'vcpkg.exe'
if (-not (Test-Path $vcpkg)) {
    Invoke-Checked (Join-Path $vcpkgDir 'bootstrap-vcpkg.bat') @('-disableMetrics')
}

Write-Host "== 3/4 FFmpeg [$Features], triplet $Triplet (first run takes a while)"
Invoke-Checked $vcpkg @('install', "ffmpeg[$Features]:$Triplet")

Write-Host "== 4/4 check"
$installed = Join-Path $vcpkgDir "installed\$Triplet"
$features = (& $vcpkg list ffmpeg) -join "`n"
Write-Host $features
if ($features -notmatch 'nvcodec') { throw 'ffmpeg was installed without the nvcodec feature.' }
foreach ($f in @('lib\avformat.lib', 'lib\avcodec.lib', 'lib\avutil.lib', 'include\ffnvcodec\nvEncodeAPI.h')) {
    if (-not (Test-Path (Join-Path $installed $f))) { throw "Expected file missing: $installed\$f" }
}
Get-ChildItem (Join-Path $installed 'bin') -Filter 'av*.dll' | ForEach-Object { Write-Host "   $($_.Name)" }

Write-Host ''
Write-Host 'Done. Open the parallax folder in VS Code and pick the windows-release preset, or from a'
Write-Host 'Developer PowerShell in that folder:'
Write-Host '  cmake --preset windows-release'
Write-Host '  cmake --build --preset windows-release'
Write-Host '  build\windows-release\parallax-probe.exe rtsp://<camera>/<stream>'
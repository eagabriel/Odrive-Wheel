param(
  [string]$ToolchainBin = "C:\Program Files (x86)\GNU Arm Embedded Toolchain\9 2020-q2-update\bin",
  [string]$MsysBash = "C:\msys64\usr\bin\bash.exe",
  [switch]$NoClean
)

$ErrorActionPreference = "Stop"

if (!(Test-Path $MsysBash)) {
  Write-Error "MSYS2 bash not found at $MsysBash"
  exit 1
}

if (!(Test-Path $ToolchainBin)) {
  Write-Error "ARM toolchain bin path not found: $ToolchainBin"
  exit 1
}

function Convert-ToMsysPath {
  param([string]$Path)
  $full = (Resolve-Path $Path).Path
  $drive, $rest = $full -split ":", 2
  $drive = $drive.ToLower()
  $rest = $rest -replace "\\", "/"
  return "/$drive$rest"
}

$repoRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$firmwareDir = Join-Path $repoRoot "Odrive-Wheel"

$msysToolchain = Convert-ToMsysPath $ToolchainBin
$msysFirmware = Convert-ToMsysPath $firmwareDir

$cmdLines = @(
  ('export PATH="{0}":$PATH' -f $msysToolchain),
  ('cd "{0}"' -f $msysFirmware)
)

if (-not $NoClean) {
  $cmdLines += "/mingw64/bin/mingw32-make clean"
}

$cmdLines += '/mingw64/bin/mingw32-make -j$(nproc)'
$cmd = ($cmdLines -join "`n")

Write-Host "Running build in $firmwareDir"
& $MsysBash -lc $cmd

if ($LASTEXITCODE -ne 0) {
  Write-Error "Build failed with exit code $LASTEXITCODE"
  exit $LASTEXITCODE
}

$binPath = Join-Path $firmwareDir "build\odrive-wheel.bin"
if (!(Test-Path $binPath)) {
  Write-Error "Missing build output: $binPath"
  exit 1
}

Write-Host "Build OK: $binPath"

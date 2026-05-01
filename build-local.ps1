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
$odriveRoot = Join-Path $repoRoot "ODrive-fw-v0.5.6\ODrive-fw-v0.5.6"
$odriveFirmwareDir = Join-Path $odriveRoot "Firmware"
$autogenDir = Join-Path $odriveFirmwareDir "autogen"

$toolsRoot = Join-Path $odriveRoot "tools"
$odriveTools = Join-Path $toolsRoot "odrive"
$fibreTools = Join-Path $toolsRoot "fibre-tools"
$versionScript = Join-Path $odriveTools "version.py"
$interfaceGenerator = Join-Path $fibreTools "interface_generator.py"

function Ensure-ToolFile {
  param(
    [string]$Path,
    [string]$Url
  )
  if (!(Test-Path $Path)) {
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $Path) | Out-Null
    Invoke-WebRequest -Uri $Url -OutFile $Path
  }
}

Ensure-ToolFile -Path $versionScript -Url "https://raw.githubusercontent.com/odriverobotics/ODrive/master/tools/odrive/version.py"
Ensure-ToolFile -Path $interfaceGenerator -Url "https://raw.githubusercontent.com/odriverobotics/ODrive/master/tools/fibre-tools/interface_generator.py"

$venvPython = Join-Path $repoRoot ".venv\bin\python.exe"
$venvPythonWin = Join-Path $repoRoot ".venv\Scripts\python.exe"
if (Test-Path $venvPython) {
  $pythonExe = $venvPython
} elseif (Test-Path $venvPythonWin) {
  $pythonExe = $venvPythonWin
} else {
  $pythonCmd = Get-Command python -ErrorAction SilentlyContinue
  if (-not $pythonCmd) {
    $pythonCmd = Get-Command python3 -ErrorAction SilentlyContinue
  }

  if (-not $pythonCmd) {
    Write-Error "Python 3 not found. Install Python and pip packages: pyyaml, jinja2, jsonschema==4.17.3"
    exit 1
  }

  $pythonExe = $pythonCmd.Source
}

& $pythonExe -c "import yaml, jinja2, jsonschema" 2>$null
if ($LASTEXITCODE -ne 0) {
  Write-Host "Installing Python deps..."
  & $pythonExe -m pip install pyyaml jinja2 jsonschema==4.17.3
  if ($LASTEXITCODE -ne 0) {
    Write-Error "Failed to install Python deps. Run: pip install pyyaml jinja2 jsonschema==4.17.3"
    exit 1
  }
}

Push-Location $odriveFirmwareDir
New-Item -ItemType Directory -Force -Path "autogen" | Out-Null

& $pythonExe $versionScript --output autogen/version.c
if ($LASTEXITCODE -ne 0) { Pop-Location; Write-Error "Failed to generate autogen/version.c"; exit 1 }

& $pythonExe interface_generator_stub.py --definitions odrive-interface.yaml --template fibre-cpp/interfaces_template.j2 --output autogen/interfaces.hpp
if ($LASTEXITCODE -ne 0) { Pop-Location; Write-Error "Failed to generate autogen/interfaces.hpp"; exit 1 }

& $pythonExe interface_generator_stub.py --definitions odrive-interface.yaml --template fibre-cpp/function_stubs_template.j2 --output autogen/function_stubs.hpp
if ($LASTEXITCODE -ne 0) { Pop-Location; Write-Error "Failed to generate autogen/function_stubs.hpp"; exit 1 }

& $pythonExe interface_generator_stub.py --definitions odrive-interface.yaml --generate-endpoints ODrive --template fibre-cpp/endpoints_template.j2 --output autogen/endpoints.hpp
if ($LASTEXITCODE -ne 0) { Pop-Location; Write-Error "Failed to generate autogen/endpoints.hpp"; exit 1 }

& $pythonExe interface_generator_stub.py --definitions odrive-interface.yaml --template fibre-cpp/type_info_template.j2 --output autogen/type_info.hpp
if ($LASTEXITCODE -ne 0) { Pop-Location; Write-Error "Failed to generate autogen/type_info.hpp"; exit 1 }

Pop-Location

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

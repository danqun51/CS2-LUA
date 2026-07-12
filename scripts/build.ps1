param(
  [ValidateSet("Debug", "Release")]
  [string]$Config = "Release"
)

$ErrorActionPreference = "Stop"
$VsRoot = "E:\Visual Studio\2022\Community"
$VcVars = Join-Path $VsRoot "VC\Auxiliary\Build\vcvars64.bat"
$CMake = Join-Path $VsRoot "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
$Root = (Resolve-Path (Join-Path $PSScriptRoot ".."))

if (!(Test-Path $VcVars)) { throw "vcvars64.bat not found: $VcVars" }
if (!(Test-Path $CMake)) { throw "cmake.exe not found: $CMake" }

Push-Location $Root
try {
  cmd /c "`"$VcVars`" && `"$CMake`" -S . -B build -G `"Visual Studio 17 2022`" -A x64 && `"$CMake`" --build build --config $Config"
  if ($LASTEXITCODE -ne 0) { throw "build failed: $LASTEXITCODE" }
} finally {
  Pop-Location
}

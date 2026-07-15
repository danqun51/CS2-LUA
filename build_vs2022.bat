@echo off
setlocal
set "VS_ROOT=E:\Visual Studio\2022\Community"
set "VCVARS=%VS_ROOT%\VC\Auxiliary\Build\vcvars64.bat"
set "CMAKE=%VS_ROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
set "ROOT=%~dp0."

if not exist "%VCVARS%" (
  echo [ERROR] vcvars64.bat not found: %VCVARS%
  exit /b 1
)
if not exist "%CMAKE%" (
  echo [ERROR] cmake.exe not found: %CMAKE%
  exit /b 1
)

call "%VCVARS%"
pushd "%ROOT%"
"%CMAKE%" -S . -B build -G "Visual Studio 17 2022" -A x64 || exit /b 1
"%CMAKE%" --build build --config Release || exit /b 1
popd
endlocal

@echo off
setlocal

set "VS_ROOT=E:\Visual Studio\2022\Community"
set "VCVARS=%VS_ROOT%\VC\Auxiliary\Build\vcvars64.bat"
set "ROOT=%~dp0."

if not exist "%VCVARS%" (
    echo [error] vcvars64.bat not found: "%VCVARS%"
    exit /b 1
)

call "%VCVARS%" >nul || exit /b 1

cl /nologo /utf-8 /std:c++17 /EHsc /LD ^
    /I"%ROOT%" ^
    /Fe"%ROOT%\FuckVacAgain_reconstructed.dll" ^
    "%ROOT%\FuckVacAgain_reconstructed.cpp" ^
    /link /NOLOGO

pause
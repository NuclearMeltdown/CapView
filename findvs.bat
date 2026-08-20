@echo off
rem Locates Visual Studio and imports the x64 build environment into the caller.
rem Sets VSPATH and VSCMAKE. Exits with 1 when no usable installation is found.

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
  echo [FEHLER] vswhere.exe nicht gefunden. Ist Visual Studio 2022 installiert?
  exit /b 1
)

set "VSLIST=%TEMP%\capview_vspath.txt"
"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath > "%VSLIST%"

set "VSPATH="
set /p VSPATH=<"%VSLIST%"
del "%VSLIST%" >nul 2>&1

if not defined VSPATH (
  echo [FEHLER] Keine Visual-Studio-Installation mit C++-Buildtools gefunden.
  echo          Im Visual Studio Installer die Workload "Desktopentwicklung mit C++" nachinstallieren.
  exit /b 1
)

if not exist "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" (
  echo [FEHLER] vcvars64.bat fehlt in "%VSPATH%".
  exit /b 1
)

rem VS eigene Skripte geben hier harmlose Meldungen auf stderr aus, daher beides nach nul.
call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
where cl >nul 2>&1
if errorlevel 1 (
  echo [FEHLER] Die Buildumgebung konnte nicht geladen werden - cl.exe nicht gefunden.
  exit /b 1
)

set "VSCMAKE=%VSPATH%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
if not exist "%VSCMAKE%" set "VSCMAKE=cmake"
exit /b 0

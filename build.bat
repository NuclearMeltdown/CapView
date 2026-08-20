@echo off
rem Baut CapView. Optional: "build.bat debug" fuer einen Debug-Build.
setlocal
set "ROOT=%~dp0"
if "%ROOT:~-1%"=="\" set "ROOT=%ROOT:~0,-1%"

set "CFG=Release"
if /i "%~1"=="debug" set "CFG=Debug"

call "%ROOT%\findvs.bat"
if errorlevel 1 exit /b 1

set "NINJA=%VSPATH%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"

echo === Konfiguriere (%CFG%) ===
if exist "%NINJA%" (
  "%VSCMAKE%" -S "%ROOT%" -B "%ROOT%\build" -G Ninja -DCMAKE_MAKE_PROGRAM="%NINJA%" -DCMAKE_BUILD_TYPE=%CFG%
) else (
  "%VSCMAKE%" -S "%ROOT%" -B "%ROOT%\build" -G Ninja -DCMAKE_BUILD_TYPE=%CFG%
)
if errorlevel 1 exit /b 1

echo === Baue ===
"%VSCMAKE%" --build "%ROOT%\build" --parallel
if errorlevel 1 exit /b 1

echo.
echo === Fertig: %ROOT%\build\bin\CapView.exe ===
exit /b 0

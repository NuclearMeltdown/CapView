@echo off
rem Baut CapView.
rem
rem   build.bat            Release, raeumt danach auf: es bleibt nur CapView.exe
rem   build.bat keep       Release, behaelt den build-Ordner fuer schnelle Neubauten
rem   build.bat debug      Debug-Build, behaelt den build-Ordner
setlocal
set "ROOT=%~dp0"
if "%ROOT:~-1%"=="\" set "ROOT=%ROOT:~0,-1%"

set "CFG=Release"
set "CLEAN=1"
if /i "%~1"=="debug" set "CFG=Debug"
if /i "%~1"=="debug" set "CLEAN=0"
if /i "%~1"=="keep" set "CLEAN=0"

call "%ROOT%\findvs.bat"
if errorlevel 1 exit /b 1

set "NINJA=%VSPATH%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"

echo === Konfiguriere (%CFG%) ===
if not exist "%NINJA%" goto :nonijna
"%VSCMAKE%" -S "%ROOT%" -B "%ROOT%\build" -G Ninja -DCMAKE_MAKE_PROGRAM="%NINJA%" -DCMAKE_BUILD_TYPE=%CFG%
goto :configured
:nonijna
"%VSCMAKE%" -S "%ROOT%" -B "%ROOT%\build" -G Ninja -DCMAKE_BUILD_TYPE=%CFG%
:configured
if errorlevel 1 exit /b 1

echo === Baue ===
"%VSCMAKE%" --build "%ROOT%\build" --parallel
if errorlevel 1 exit /b 1

if not exist "%ROOT%\build\bin\CapView.exe" goto :missing

copy /y "%ROOT%\build\bin\CapView.exe" "%ROOT%\CapView.exe" >nul
if errorlevel 1 exit /b 1

rem Die Medienquelle der virtuellen Kamera muss neben der exe liegen -- dort sucht
rem CapView sie, und dorthin zeigt die Registrierung nach dem Installieren.
rem
rem Der Fehlschlag hier ist kein Randfall: Windows haelt die DLL fest, solange die
rem Kamera irgendwo benutzt wird oder wurde. Wird das verschluckt, laeuft eine neue
rem exe gegen eine alte Quelle und das Bild ist einfach schwarz.
if not exist "%ROOT%\build\bin\capview_vcam.dll" goto :novcam
copy /y "%ROOT%\build\bin\capview_vcam.dll" "%ROOT%\capview_vcam.dll" >nul
if errorlevel 1 (
  echo.
  echo [WARNUNG] capview_vcam.dll liess sich nicht ersetzen - vermutlich haelt der
  echo           Frame Server sie fest. Programme schliessen, die die Kamera nutzen,
  echo           dann erneut bauen. Bis dahin passen exe und Kameraquelle nicht.
  echo.
)
:novcam

if "%CLEAN%"=="0" goto :kept

rem Alles retten, was zufaellig neben der exe liegt, bevor der Ordner faellt:
rem der ffmpeg-Download landet dort, und die Einstellungen tun es auch.
if not exist "%ROOT%\build\bin\ffmpeg" goto :noffmpeg
if exist "%ROOT%\ffmpeg" goto :noffmpeg
echo === Verschiebe ffmpeg neben die exe ===
move "%ROOT%\build\bin\ffmpeg" "%ROOT%\ffmpeg" >nul
:noffmpeg
if not exist "%ROOT%\build\bin\CapView.json" goto :nojson
if exist "%ROOT%\CapView.json" goto :nojson
move "%ROOT%\build\bin\CapView.json" "%ROOT%\CapView.json" >nul
:nojson

echo === Raeume auf ===
rmdir /s /q "%ROOT%\build"

echo.
echo === Fertig: %ROOT%\CapView.exe ===
echo Einstellungen landen in CapView.json daneben. Sonst wird nichts angelegt.
exit /b 0

:kept
echo.
echo === Fertig: %ROOT%\CapView.exe   build-Ordner behalten ===
exit /b 0

:missing
echo FEHLER: CapView.exe wurde nicht erzeugt.
exit /b 1

@echo off
rem QODE release build on Windows (MSVC + Qt msvc2022_64 kit).
rem
rem   scripts\win\compile.bat [noweb] [extra qmake args...]
rem
rem   noweb   build without Qt WebEngine (CONFIG+=no_webengine), into build-win-noweb\
rem           (default: with WebEngine, into build-win\)
rem   extra qmake args must be quoted, because cmd splits arguments at "=":  "DEFINES+=FOO"
rem
rem Finds everything by itself, no paths to type:
rem   Qt      %QODE_QMAKE% if set, else the newest <QODE_QT_ROOT>\6.*\msvc2022_64\bin\qmake6.exe
rem           (QODE_QT_ROOT defaults to D:\qt)
rem   MSVC    already set up (cl on PATH), else vcvars64.bat of the newest Visual Studio found by vswhere
rem   make    jom (PATH or <Qt root>\Tools\jom\jom.exe), else nmake; %QODE_MAKEFLAGS% is passed to it (/K)
rem
rem The result is <build folder>\qode.exe (or <build folder>\release\qode.exe), with the Qt DLLs and plugins copied
rem next to it by windeployqt, so it starts with a double-click.

setlocal EnableExtensions DisableDelayedExpansion

for %%I in ("%~dp0..\..") do set "ROOT=%%~fI"

set "FLAVOR=web"
set "EXTRA="
:parse
if "%~1"=="" goto parsed
if /I "%~1"=="noweb" (
    set "FLAVOR=noweb"
) else if /I "%~1"=="-h" (
    goto usage
) else if /I "%~1"=="--help" (
    goto usage
) else (
    call set "EXTRA=%%EXTRA%% "%~1""
)
shift
goto parse
:parsed

if /I "%FLAVOR%"=="noweb" (
    set "BUILD=%ROOT%\build-win-noweb"
    set "FLAVOR_ARG=CONFIG+=no_webengine"
) else (
    set "BUILD=%ROOT%\build-win"
    set "FLAVOR_ARG="
)

rem ---- Qt --------------------------------------------------------------------------------------------------
if defined QODE_QMAKE (
    set "QMAKE=%QODE_QMAKE%"
    goto haveqmake
)
if not defined QODE_QT_ROOT set "QODE_QT_ROOT=D:\qt"
set "QMAKE="
rem Name order: fine while one 6.x is installed; set QODE_QMAKE to pick one explicitly.
for /f "delims=" %%V in ('dir /b /ad /o:n "%QODE_QT_ROOT%\6.*" 2^>nul') do (
    if exist "%QODE_QT_ROOT%\%%V\msvc2022_64\bin\qmake6.exe" set "QMAKE=%QODE_QT_ROOT%\%%V\msvc2022_64\bin\qmake6.exe"
)
:haveqmake
if not defined QMAKE goto noqt
if not exist "%QMAKE%" goto noqt
for %%I in ("%QMAKE%") do set "QTBIN=%%~dpI"
set "QTBIN=%QTBIN:~0,-1%"
for %%I in ("%QTBIN%\..\..\..") do set "QTROOT=%%~fI"

rem ---- MSVC ------------------------------------------------------------------------------------------------
where cl >nul 2>nul && goto havecl
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" goto novs
set "VSDIR="
for /f "usebackq delims=" %%P in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%P"
if not defined VSDIR goto novs
if not exist "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" goto novs
rem vcvars runs vswhere by name itself.
for %%I in ("%VSWHERE%") do set "PATH=%PATH%;%%~dpI"
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul
where cl >nul 2>nul || goto novs
:havecl

rem ---- make tool -------------------------------------------------------------------------------------------
rem nmake runs one job, but Qt's mkspec passes -MP, so cl still compiles each batch of files on all cores.
set "MAKE="
for /f "delims=" %%J in ('where jom 2^>nul') do if not defined MAKE set "MAKE=%%J"
if not defined MAKE if exist "%QTROOT%\Tools\jom\jom.exe" set "MAKE=%QTROOT%\Tools\jom\jom.exe"
if not defined MAKE set "MAKE=nmake"

rem ---- build folder ----------------------------------------------------------------------------------------
rem A folder configured by another qmake (another Qt) must not be mixed: stop and say so, never delete it here.
if not exist "%BUILD%" mkdir "%BUILD%"
set "STAMP=%BUILD%\.qode-qmake"
if exist "%STAMP%" (
    set /p OLDQMAKE=<"%STAMP%"
) else (
    set "OLDQMAKE="
)
if defined OLDQMAKE if /I not "%OLDQMAKE%"=="%QMAKE%" goto otherqt

echo.
echo  QODE build (%FLAVOR%)
echo    Qt     %QMAKE%
echo    make   %MAKE%
echo    into   %BUILD%
echo.

pushd "%BUILD%" || exit /b 1
"%QMAKE%" "%ROOT%\QODE.pro" -spec win32-msvc "CONFIG+=release" "CONFIG-=debug_and_release" %FLAVOR_ARG% %EXTRA%
if errorlevel 1 (
    popd
    echo.
    echo  qmake failed.
    exit /b 1
)
>"%STAMP%" echo %QMAKE%
>"%BUILD%\.qode-qtbin" echo %QTBIN%
rem QODE_MAKEFLAGS: extra flags for jom / nmake, e.g. /K to go on after errors and see all of them.
"%MAKE%" %QODE_MAKEFLAGS%
if errorlevel 1 (
    popd
    echo.
    echo  Build failed.
    exit /b 1
)
popd

set "EXE="
if exist "%BUILD%\release\qode.exe" set "EXE=%BUILD%\release\qode.exe"
if exist "%BUILD%\qode.exe" set "EXE=%BUILD%\qode.exe"
if not defined EXE (
    echo  Build finished but qode.exe was not found in %BUILD%.
    exit /b 1
)
rem ---- Qt DLLs next to the exe ------------------------------------------------------------------------------
rem Windows looks for DLLs next to the exe and on PATH, so without this a double-clicked qode.exe stops with
rem "Qt6Core.dll was not found". windeployqt copies only what changed, so repeated builds stay quick.
rem Set QODE_NODEPLOY=1 to skip it (run.bat still works then: it puts Qt's bin\ on PATH).
if defined QODE_NODEPLOY goto deployed
echo.
echo  Copying the Qt DLLs and plugins next to qode.exe (windeployqt)...
"%QTBIN%\windeployqt.exe" --release --no-translations --no-compiler-runtime --verbose 0 "%EXE%"
if errorlevel 1 (
    echo  windeployqt failed: qode.exe only starts through run.bat.
    exit /b 1
)
:deployed

echo.
echo  Built %EXE%
exit /b 0

:usage
echo usage: scripts\win\compile.bat [noweb] ["extra qmake arg" ...]
echo   noweb   build without Qt WebEngine into build-win-noweb\ (default: build-win\)
echo   quote qmake args, e.g. "DEFINES+=FOO": cmd splits arguments at "="
exit /b 0

:noqt
echo  Qt not found. Install Qt 6 msvc2022_64 under %QODE_QT_ROOT% or set QODE_QMAKE to its bin\qmake6.exe.
exit /b 1

:novs
echo  MSVC not found. Install Visual Studio (or its Build Tools) with "Desktop development with C++".
exit /b 1

:otherqt
echo  %BUILD% was configured by another Qt:
echo    %OLDQMAKE%
echo  Delete that folder by hand (or set QODE_QMAKE back), then build again.
exit /b 1

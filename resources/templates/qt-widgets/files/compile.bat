@echo off
rem Builds {{name}} into build\{{nameId}}.exe.
setlocal EnableDelayedExpansion
cd /d "%~dp0"

rem The Qt kit this project was created with; set QMAKE to another kit's qmake.exe to use that one.
if not defined QMAKE set "QMAKE={{qmake}}"
if not exist "%QMAKE%" (
    echo Qt's qmake.exe was not found at "%QMAKE%". Install Qt 6 with the Qt Online Installer, or set QMAKE.
    exit /b 1
)
for %%q in ("%QMAKE%") do set "QTBIN=%%~dpq"
for %%r in ("%QTBIN%..\..\..") do set "QTROOT=%%~fr"
for %%k in ("%QTBIN%..") do set "QTPREFIX=%%~fk"
set "PATH=%QTBIN%;%PATH%"
rem The Qt installer's own CMake and Ninja, when it has them.
if exist "%QTROOT%\Tools\CMake_64\bin\cmake.exe" set "PATH=%QTROOT%\Tools\CMake_64\bin;%PATH%"
if exist "%QTROOT%\Tools\Ninja\ninja.exe" set "PATH=%QTROOT%\Tools\Ninja;%PATH%"

rem The compiler the kit was built with: Visual Studio for an msvc kit, the kit's MinGW for a mingw one.
echo "%QTBIN%" | findstr /i "msvc" >nul
rem (!…! inside the blocks: a folder name with a parenthesis would end a block expanded with %…%.)
if not errorlevel 1 (
    where cl >nul 2>nul || call :vcvars || exit /b 1
    set "MAKE=nmake /nologo"
    if exist "!QTROOT!\Tools\jom\jom.exe" set "MAKE="!QTROOT!\Tools\jom\jom.exe""
    set "GENERATOR=NMake Makefiles"
) else (
    for /d %%m in ("!QTROOT!\Tools\mingw*") do if exist "%%~fm\bin\mingw32-make.exe" set "PATH=%%~fm\bin;!PATH!"
    set "MAKE=mingw32-make -j!NUMBER_OF_PROCESSORS!"
    set "GENERATOR=MinGW Makefiles"
)

if not exist build md build
cd build || exit /b 1
{{#if buildSystem=qmake}}
rem One debug build straight into build\ (no debug\ and release\ folders).
"%QMAKE%" ..\{{nameId}}.pro "CONFIG+=debug" "CONFIG-=debug_and_release" || exit /b 1
%MAKE% || exit /b 1
{{/if}}
{{#if buildSystem=CMake}}
where ninja >nul 2>nul && set "GENERATOR=Ninja"
if not exist CMakeCache.txt (
    cmake .. -G "!GENERATOR!" -DCMAKE_BUILD_TYPE=Debug "-DCMAKE_PREFIX_PATH=%QTPREFIX%" || exit /b 1
)
cmake --build . --parallel || exit /b 1
{{/if}}
exit /b 0

rem Finds the newest Visual Studio with the C++ tools (vswhere) and runs its vcvars64.bat in this window.
:vcvars
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" goto :novs
set "VSDIR="
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR goto :novs
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1 || goto :novs
exit /b 0
:novs
echo This Qt kit needs Visual Studio's C++ tools, which were not found. Install the Visual Studio Build Tools with
echo "Desktop development with C++", or use a MinGW kit of Qt.
exit /b 1

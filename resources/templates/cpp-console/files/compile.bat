@echo off
rem Builds {{name}} into build\{{nameId}}.exe.   Usage: compile.bat [test]
setlocal EnableDelayedExpansion
cd /d "%~dp0"
{{#if compiler=msvc}}

rem MSVC: Visual Studio's compiler, set up by its vcvars64.bat unless this window already has it.
where cl >nul 2>nul || call :vcvars || exit /b 1
{{/if}}
{{#if buildSystem=CMake}}
{{#if compiler=msvc}}
set "CXX=cl"
set "GENERATOR=NMake Makefiles"
{{/if}}
{{#if !compiler=msvc}}
if not defined CXX set "CXX={{compiler}}"
set "GENERATOR=MinGW Makefiles"
{{/if}}
rem Ninja when it is installed (Visual Studio and the Qt installer bring one): faster than the makefiles.
where ninja >nul 2>nul && set "GENERATOR=Ninja"
if not defined BUILD_TYPE set "BUILD_TYPE=Debug"
if not exist build\CMakeCache.txt (
    cmake -S . -B build -G "!GENERATOR!" -DCMAKE_BUILD_TYPE=%BUILD_TYPE% || exit /b 1
)
cmake --build build --parallel || exit /b 1
{{#if tests}}
if "%~1"=="test" (
    ctest --test-dir build --output-on-failure || exit /b 1
)
{{/if}}
exit /b 0
{{/if}}
{{#if buildSystem=Scripts}}
if not exist build\obj md build\obj

rem Every .cpp under src\ is compiled and linked again: small programs build in a moment.
set "SOURCES="
set "LIBRARY="
for /r src %%f in (*.cpp) do (
    set SOURCES=!SOURCES! "%%f"
    if /i not "%%~nxf"=="main.cpp" set LIBRARY=!LIBRARY! "%%f"
)
{{#if compiler=msvc}}
{{#if std=23}}
set "STD=/std:c++latest"
{{/if}}
{{#if !std=23}}
set "STD=/std:c++{{std}}"
{{/if}}
set "FLAGS=/nologo /MP %STD% /EHsc /permissive- /W4 /Zi /Iinclude /Fobuild\obj\ /Fdbuild\obj\"
echo   CL  src\*.cpp
cl %FLAGS% /Febuild\{{nameId}}.exe !SOURCES! || exit /b 1
{{#if tests}}

if "%~1"=="test" (
    cl %FLAGS% /Febuild\tests.exe !LIBRARY! tests\test_main.cpp || exit /b 1
    build\tests.exe || exit /b 1
)
{{/if}}
{{/if}}
{{#if !compiler=msvc}}
if not defined CXX set "CXX={{compiler}}"
set "COMPILE=%CXX%"
{{#if ccache}}

rem ccache: reuse earlier compiler output for unchanged code.
where ccache >nul 2>nul && set "COMPILE=ccache %CXX%"
{{/if}}
echo   CXX  src\*.cpp
%COMPILE% -std=c++{{std}} -Wall -Wextra -Wpedantic -g -Iinclude !SOURCES! -o build\{{nameId}}.exe || exit /b 1
{{#if tests}}

if "%~1"=="test" (
    %COMPILE% -std=c++{{std}} -Wall -Wextra -Wpedantic -g -Iinclude !LIBRARY! tests\test_main.cpp -o build\tests.exe || exit /b 1
    build\tests.exe || exit /b 1
)
{{/if}}
{{/if}}
exit /b 0
{{/if}}
{{#if compiler=msvc}}

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
echo Visual Studio's C++ tools were not found. Install the Visual Studio Build Tools with "Desktop development with C++".
exit /b 1
{{/if}}

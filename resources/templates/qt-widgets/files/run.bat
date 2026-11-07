@echo off
rem Builds what changed, then runs {{name}} with Qt's DLLs on PATH.   Usage: run.bat [arguments for the program]
setlocal
cd /d "%~dp0"

call "%~dp0compile.bat" || exit /b 1
if not defined QMAKE set "QMAKE={{qmake}}"
for %%q in ("%QMAKE%") do set "QTBIN=%%~dpq"
for %%r in ("%QTBIN%..\..\..") do set "QTROOT=%%~fr"
rem Qt's DLLs, and for a MinGW kit its compiler's runtime DLLs.
echo "%QTBIN%" | findstr /i "msvc" >nul || for /d %%m in ("%QTROOT%\Tools\mingw*") do set "PATH=%%~fm\bin;%PATH%"
set "PATH=%QTBIN%;%PATH%"
build\{{nameId}}.exe %*

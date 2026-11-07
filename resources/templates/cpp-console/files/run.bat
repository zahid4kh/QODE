@echo off
rem Builds what changed, then runs {{name}}.   Usage: run.bat [arguments for the program]
setlocal
cd /d "%~dp0"

call "%~dp0compile.bat" || exit /b 1
build\{{nameId}}.exe %*

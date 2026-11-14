@echo off
rem Rebuilds the exe and regenerates the website screenshots in docs\img.
rem Needs MinGW-w64 on PATH, or at C:\msys64\mingw64\bin.
cd /d "%~dp0.."
where gcc >nul 2>nul || set "PATH=%PATH%;C:\msys64\mingw64\bin"
taskkill /f /im WindowFocusHighlighter.exe >nul 2>nul
call "%~dp0..\build.bat" || exit /b 1
powershell -NoProfile -ExecutionPolicy Bypass -File tools\screenshots.ps1

@echo off
rem Requires MinGW-w64 gcc + windres on PATH (e.g. C:\msys64\mingw64\bin)
windres app.rc -O coff -o app_res.o || exit /b 1
gcc -Os -fno-math-errno -municode -mwindows -s -Wall -Wl,--gc-sections highlighter.c app_res.o -o WindowHighlighter.exe -ldwmapi -lcomctl32 -lcomdlg32 -lshell32 -luser32 -lgdi32 -luxtheme -ladvapi32

@echo off
rem Rebuild the game only. Output: build_log2.txt
cd /d "%~dp0"
taskkill /F /IM spiderman_recomp.exe >nul 2>&1
"C:\Program Files\CMake\bin\cmake.exe" --build build --config Release --parallel 4 > build_log2.txt 2>&1
if errorlevel 1 (echo BUILD FAILED >> build_log2.txt) else (echo BUILD OK >> build_log2.txt)

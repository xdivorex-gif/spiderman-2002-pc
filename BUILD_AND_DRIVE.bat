@echo off
rem Rebuild (about 30 s), then play with a screen capture every 2 s into cap\wh\
cd /d "%~dp0"
taskkill /F /IM spiderman_recomp.exe >nul 2>&1
echo Building, about 30 seconds...
"C:\Program Files\CMake\bin\cmake.exe" --build build --config Release --parallel 4 > build_log2.txt 2>&1
if errorlevel 1 (
  echo BUILD FAILED >> build_log2.txt
  echo Build failed - tell Claude.
  pause
  exit /b 1
)
echo BUILD OK >> build_log2.txt
del /q cap\drv\*.bmp >nul 2>&1
type nul > pad_live.txt
call play_drive.bat

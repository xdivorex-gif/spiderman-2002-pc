@echo off
rem Rebuild, then the automatic sound test with the ISR stall skipped; audio recorded to audio_nostall.wav
cd /d "%~dp0"
taskkill /F /IM spiderman_recomp.exe >nul 2>&1
"C:\Program Files\CMake\bin\cmake.exe" --build build --config Release --parallel 4 > build_log2.txt 2>&1
if errorlevel 1 (
  echo BUILD FAILED >> build_log2.txt
  exit /b 1
)
echo BUILD OK >> build_log2.txt
set RECOMP_ISR_NO_STALL=1
set RECOMP_AUDIO_WAV=audio_nostall.wav
call sound_test_auto.bat

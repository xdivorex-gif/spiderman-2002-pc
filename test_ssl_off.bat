@echo off
rem Rebuild, then the automatic sound test with the APU SSL tick OFF; audio recorded to audio_ssl_off.wav
cd /d "%~dp0"
taskkill /F /IM spiderman_recomp.exe >nul 2>&1
"C:\Program Files\CMake\bin\cmake.exe" --build build --config Release --parallel 4 > build_log2.txt 2>&1
if errorlevel 1 (
  echo BUILD FAILED >> build_log2.txt
  exit /b 1
)
echo BUILD OK >> build_log2.txt
set RECOMP_APU_SSL_TICK=0
set RECOMP_AUDIO_WAV=audio_ssl_off.wav
call sound_test_auto.bat

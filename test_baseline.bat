@echo off
rem Rebuild, then the automatic sound test with the APU SSL tick ON (as before); audio recorded to audio_baseline.wav
cd /d "%~dp0"
taskkill /F /IM spiderman_recomp.exe >nul 2>&1

set RECOMP_AUDIO_WAV=audio_baseline.wav
call sound_test_auto.bat

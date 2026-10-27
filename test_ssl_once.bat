@echo off
rem Rebuild, then the automatic sound test with the APU SSL tick ONCE; audio recorded to audio_ssl_once.wav
cd /d "%~dp0"
taskkill /F /IM spiderman_recomp.exe >nul 2>&1
set RECOMP_APU_SSL_TICK=once
set RECOMP_AUDIO_WAV=audio_ssl_once.wav
call sound_test_auto.bat

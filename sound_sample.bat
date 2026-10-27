@echo off
rem Sound test plus a 5 s stack sample of every thread, 70 s in (sample_stacks.txt).
cd /d "%~dp0"
taskkill /F /IM spiderman_recomp.exe >nul 2>&1
set RECOMP_SAMPLE=70
set RECOMP_SAMPLE_ALL=1
call sound_test_auto.bat

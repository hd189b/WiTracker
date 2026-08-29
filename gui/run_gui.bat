@echo off
cd /d "%~dp0"
py -m pip install -r requirements.txt
if errorlevel 1 (
  echo Failed to install pyserial.
  pause
  exit /b 1
)
py localization_gui.py
pause

@echo off
setlocal
cd /d "%~dp0"

echo [1/3] clearing stale listeners on 8765 / 8766 ...
for /f "tokens=5" %%p in ('netstat -ano ^| findstr ":8765 :8766" ^| findstr LISTENING') do taskkill /f /pid %%p >nul 2>&1

echo [2/3] starting page server + AI bridge ...
start "P4X-web" /min "C:\py312\python.exe" serve.py 8765
start "P4X-ai"  /min "C:\py312\python.exe" ai_bridge.py 8766
timeout /t 3 >nul

echo [3/3] opening dashboard ...
start "" http://localhost:8765/dashboard.html
timeout /t 8 >nul
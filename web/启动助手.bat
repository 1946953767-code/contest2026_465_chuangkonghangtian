@echo off
setlocal
cd /d "%~dp0"

echo [1/4] clearing stale listeners on 8765 / 8766 ...
for /f "tokens=5" %%p in ('netstat -ano ^| findstr ":8765 :8766" ^| findstr LISTENING') do (
    taskkill /f /pid %%p >nul 2>&1
)

echo [2/4] starting page server   -> http://localhost:8765
start "P4X-web" /min "C:\py312\python.exe" -m http.server 8765

echo [3/4] starting AI bridge     -> http://127.0.0.1:8766
start "P4X-ai" /min "C:\py312\python.exe" ai_bridge.py 8766

timeout /t 3 >nul

echo [4/4] opening dashboard ...
start "" http://localhost:8765/dashboard.html

echo.
echo   dashboard : http://localhost:8765/dashboard.html
echo   ai health : http://127.0.0.1:8766/health   (expect "key":true)
echo.
echo   keep the two minimized windows (P4X-web / P4X-ai) open.
echo   note: open via localhost, NOT file://  (WebSerial needs a secure context)
timeout /t 8 >nul
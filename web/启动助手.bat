@echo off
chcp 65001 >nul 2>&1
cd /d "%~dp0"

rem 8765：页面服务（WebSerial 需要 localhost，file:// 不行）
start "P4X web" /min "C:\py312\python.exe" -m http.server 8765

rem 8766：AI 助手代理（DeepSeek，Key 只在后端）
start "P4X AI" /min "C:\py312\python.exe" ai_bridge.py 8766

timeout /t 2 >nul
start "" http://localhost:8765/dashboard.html

echo.
echo  面板: http://localhost:8765/dashboard.html
echo  AI  : http://127.0.0.1:8766/health
echo.
echo  两个最小化窗口都别关：
echo    "P4X web" = 页面服务（localhost 才能用 WebSerial 连板子）
echo    "P4X AI"  = AI 助手（没网/没 Key 时页面会提示）
timeout /t 6 >nul

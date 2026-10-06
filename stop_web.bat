@echo off
REM ============================================================
REM  停止 synq Web 可视化服务器
REM ============================================================
echo.
echo 正在查找占用 8899 端口的进程...

for /f "tokens=5" %%P in ('netstat -ano ^| findstr :8899 ^| findstr LISTENING') do (
    echo 找到进程 PID=%%P
    taskkill /PID %%P /F >nul 2>&1
    if errorlevel 1 (
        echo   结束失败，可能需要管理员权限
    ) else (
        echo   已结束
    )
)

echo.
echo 完成。
ping -n 3 127.0.0.1 >nul
endlocal

@echo off
REM ============================================================
REM  synq 一键启动 Web 可视化
REM  双击本文件即可：自动编译 -> 生成数据 -> 启动服务器 -> 开浏览器
REM ============================================================
setlocal enabledelayedexpansion
cd /d "%~dp0"

echo.
echo ============================================================
echo   synq 帧同步回滚可视化
echo ============================================================
echo.

REM ---- 1. 检测编译器 ----
where g++ >nul 2>&1
if errorlevel 1 (
    echo [错误] 未找到 g++ 编译器
    echo.
    echo 请先安装 MSYS2: https://www.msys2.org/
    echo 安装后在 MSYS2 终端执行: pacman -S mingw-w64-x86_64-gcc
    echo.
    pause
    exit /b 1
)

REM ---- 2. 编译导出器 ----
echo [1/3] 编译导出器...
if not exist "build" mkdir build
g++ -O2 -std=c++20 -pthread -Isrc src/export_trace.cpp -o build\export_trace.exe -lws2_32 2>build\err.log
if errorlevel 1 (
    echo       编译失败：
    type build\err.log
    echo.
    pause
    exit /b 1
)
echo       完成

REM ---- 3. 生成轨迹数据 ----
echo [2/3] 生成模拟轨迹数据...
if not exist "web" mkdir web
build\export_trace.exe 600 > web\trace.json
for %%A in (web\trace.json) do echo       完成 (%%~zA 字节)

REM ---- 3.5 清理旧进程 ----
REM 上次启动的服务器可能还占着 8899，导致本次启动失败（浏览器拒绝连接）。
REM 先结束占用该端口的进程，再启动新的。
for /f "tokens=5" %%P in ('netstat -ano ^| findstr :8899 ^| findstr LISTENING') do (
    echo       清理旧服务器 PID=%%P
    taskkill /PID %%P /F >nul 2>&1
)
ping -n 2 127.0.0.1 >nul

REM ---- 4. 启动服务器 ----
REM Python 缺失兜底：尝试 py（Windows Python Launcher），都没有就用内置 node
set SERVER_CMD=
where python >nul 2>&1 && set SERVER_CMD=python
if not defined SERVER_CMD (
    where py >nul 2>&1 && set SERVER_CMD=py
)

echo [3/3] 启动服务器并打开浏览器...
echo.

REM 绑定地址说明：默认 http.server 监听 [::1]（IPv6 回环），
REM 而浏览器可能用 IPv4 访问 127.0.0.1 -> 拒绝连接。
REM 显式绑定 127.0.0.1 可同时兼容 IPv4 浏览器。
pushd "%~dp0web"
if defined SERVER_CMD (
    start "" /min %SERVER_CMD% -m http.server 8899 --bind 127.0.0.1
) else (
    echo   [提示] 未找到 Python，改用 Node.js
    where node >nul 2>&1
    if errorlevel 1 (
        echo   [错误] 既没有 Python 也没有 Node.js
        echo   请安装 Python: https://www.python.org/downloads/
        echo   或安装 Node.js: https://nodejs.org/
        echo.
        popd
        pause
        exit /b 1
    )
    REM node -e 写一个极简静态服务器
    start "" /min node -e "const h=require('http'),f=require('fs'),p=require('path');h.createServer((q,s)=>{let u=q.url==='/'?'/index.html':q.url;s.setHeader('Content-Type',u.endsWith('.json')?'application/json':'text/html; charset=utf-8');f.readFile(p.join('.',u),(e,d)=>e?s.writeHead(404).end():s.end(d))}).listen(8899,'127.0.0.1')"
)
popd

REM 等待服务器就绪：轮询端口，最多等 5 秒
set /a _try=0
:wait_loop
netstat -ano | findstr :8899 | findstr LISTENING >nul && goto ready
set /a _try+=1
if !_try! GEQ 10 (
    echo   [错误] 服务器启动失败，请检查是否安装了 Python
    pause
    exit /b 1
)
ping -n 1 127.0.0.1 >nul
goto wait_loop
:ready
echo   服务器已就绪
start "" http://127.0.0.1:8899

echo   浏览器已打开 http://localhost:8899
echo   关闭方式：双击 stop_web.bat
echo.
echo 按任意键关闭此窗口（服务器会继续运行）...
pause >nul
endlocal

@echo off
REM ============================================================
REM  synq 一键启动 Web 可视化
REM  双击本文件即可：自动编译 -> 生成轨迹 -> 起静态服务器 -> 打开浏览器
REM ============================================================
setlocal enabledelayedexpansion
cd /d "%~dp0"

echo.
echo ============================================================
echo   synq 帧同步回滚可视化
echo ============================================================
echo.

REM ---- 1. 检查编译器 ----
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

REM ---- 3.5 清理占用 8899 的旧进程 ----
for /f "tokens=5" %%P in ('netstat -ano ^| findstr :8899 ^| findstr LISTENING') do (
    echo       结束旧服务器 PID=%%P
    taskkill /PID %%P /F >nul 2>&1
)
ping -n 2 127.0.0.1 >nul

REM ---- 4. 选择可用的静态服务器 ----
REM  【关键】PATH 里的 %LOCALAPPDATA%\Microsoft\WindowsApps\python.exe 是微软商店的
REM  「应用执行别名」假存根，它不运行 Python、只会弹商店。where python 会优先命中它，
REM  所以这里优先用 py launcher；回退时显式跳过 WindowsApps 路径，
REM  并且要求解释器"真能执行 import sys"才采用。
set SERVER_CMD=
set SERVER_ARG=

call :probe "py" "-3"
if not defined SERVER_CMD for /f "delims=" %%P in ('where python 2^>nul') do (
    if not defined SERVER_CMD call :probe_path "%%P"
)
if not defined SERVER_CMD for /f "delims=" %%P in ('where python3 2^>nul') do (
    if not defined SERVER_CMD call :probe_path "%%P"
)

echo [3/3] 启动本地静态服务器...
echo.

REM  绑定地址说明：默认 http.server 会监听到 [::1]（仅 IPv6 回环），
REM  某些浏览器只连 IPv4 的 127.0.0.1 -> 拒绝连接。显式绑 127.0.0.1 更稳。
pushd "%~dp0web"
if not defined SERVER_CMD goto use_node
echo       使用解释器: %SERVER_CMD% %SERVER_ARG%
start "synq-web" /min %SERVER_CMD% %SERVER_ARG% -m http.server 8899 --bind 127.0.0.1
goto server_started

:use_node
echo   [提示] 未找到可用 Python，尝试 Node.js
where node >nul 2>&1
if errorlevel 1 goto no_server
start "synq-web" /min node -e "const h=require('http'),f=require('fs'),p=require('path');h.createServer((q,s)=>{let u=q.url==='/'?'/index.html':q.url;s.setHeader('Content-Type',u.endsWith('.json')?'application/json':'text/html; charset=utf-8');f.readFile(p.join('.',u),(e,d)=>e?s.writeHead(404).end():s.end(d))}).listen(8899,'127.0.0.1')"
goto server_started

:no_server
echo   [错误] 既没有可用的 Python 也没有 Node.js
echo   安装 Python: https://www.python.org/downloads/
echo   安装 Node.js: https://nodejs.org/
echo.
popd
pause
exit /b 1

:server_started
popd

REM  等待服务器就绪，轮询端口，最多 15 次（约 15 秒）
set /a _try=0
:wait_loop
netstat -ano | findstr :8899 | findstr LISTENING >nul && goto ready
set /a _try+=1
if !_try! GEQ 15 (
    echo.
    echo   [错误] 服务器启动失败：8899 端口始终未监听
    echo   已尝试的解释器: %SERVER_CMD% %SERVER_ARG%
    echo   排查建议：
    echo     1. 双击 stop_web.bat 释放端口后重试
    echo     2. 若使用 Microsoft Store 版 Python，请在 Windows 设置中
    echo        关闭「应用执行别名」里的 python.exe
    echo     3. 或安装官方 Python: https://www.python.org/downloads/
    echo.
    pause
    exit /b 1
)
ping -n 1 127.0.0.1 >nul
goto wait_loop
:ready
echo   [OK] 服务器已就绪
start "" http://127.0.0.1:8899

echo   请在浏览器访问 http://localhost:8899
echo   关闭方式：双击 stop_web.bat
echo.
echo 按任意键关闭此窗口（不影响已启动的服务器）...
pause >nul
endlocal
exit /b 0

REM ============ 子过程 ============
:probe
REM  %~1 = 命令或路径, %~2 = 附加固定参数（如 -3）
REM  只有真的能执行 python 代码才算可用，避免命中商店假存根
"%~1" %~2 -c "import sys" >nul 2>&1
if not errorlevel 1 (
    set SERVER_CMD="%~1"
    set SERVER_ARG=%~2
)
goto :eof

:probe_path
REM  跳过微软商店假存根（WindowsApps 下的 python.exe）
echo %~1 | findstr /i "WindowsApps" >nul
if not errorlevel 1 goto :eof
call :probe "%~1" ""
goto :eof

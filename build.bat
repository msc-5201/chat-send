@echo off
rem ============================================================
rem  build.bat —— 编译聊天服务器
rem  用法：在 test_web 目录下执行 build.bat
rem  说明：src\*.c 使用通配符收集，新增源文件无需修改本脚本。
rem        sqlite3.c 体积大、编译慢，单独缓存为 build\sqlite3.o。
rem ============================================================
setlocal enabledelayedexpansion
cd /d "%~dp0"

if not exist build mkdir build

set COMMON=-O2 -std=c11 -Iinclude -Ithird_party -DSQLITE_THREADSAFE=1
set WARN=-Wall -Wextra
set LINK=-lws2_32

if not exist build\sqlite3.o (
    echo [1/2] 编译 sqlite3.c ^(首次较慢，之后会跳过^) ...
    gcc %COMMON% -c third_party\sqlite3.c -o build\sqlite3.o
    if errorlevel 1 goto :fail
) else (
    echo [1/2] 复用已缓存的 build\sqlite3.o
)

echo [2/2] 编译 src\*.c 并链接 ...
set OBJS=
for %%f in (src\*.c) do (
    gcc %COMMON% %WARN% -c "%%f" -o "build\%%~nf.o"
    if errorlevel 1 goto :fail
    set OBJS=!OBJS! build\%%~nf.o
)

gcc -o server.exe !OBJS! build\sqlite3.o %LINK%
if errorlevel 1 goto :fail

echo.
echo 编译成功: %~dp0server.exe
echo 运行:   server.exe   然后浏览 http://localhost:8080
exit /b 0

:fail
echo.
echo *** 编译失败 ***
exit /b 1

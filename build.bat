@echo off
setlocal enableextensions
REM Build InfiniteDesktop: MSVC и Windows 10 SDK ищутся напрямую (vswhere + каталоги
REM установок) и окружение cl/link настраивается вручную. vcvars64/vsdevcmd в VS 18
REM на некоторых установках аварийно прерывают сборку (опечатка "==""set" в
REM vsdevcmd_start.bat) — прямой путь от этого не зависит.

REM ---------- Где стоит MSVC ----------
set "VS="
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if exist "%VSWHERE%" for /f "delims=" %%i in ('"%VSWHERE%" -latest -prerelease -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath') do set "VS=%%i"
if not defined VS (
    echo [!] MSVC C++ toolset not found
    exit /b 1
)

REM ---------- Самая свежая поставка тулсета и SDK ----------
set "MSV="
for /f "delims=" %%d in ('dir /b /ad /o-n "%VS%\VC\Tools\MSVC" 2^>nul') do if not defined MSV set "MSV=%%d"
if not defined MSV (
    echo [!] No VC\Tools\MSVC in the installation
    exit /b 1
)
set "MSVC=%VS%\VC\Tools\MSVC\%MSV%"

set "SDKROOT=%ProgramFiles(x86)%\Windows Kits\10"
set "SDKV="
for /f "delims=" %%d in ('dir /b /ad /o-n "%SDKROOT%\Include" 2^>nul') do if not defined SDKV set "SDKV=%%d"
if not defined SDKV (
    echo [!] Windows 10 SDK not found
    exit /b 1
)
set "INCD=%SDKROOT%\Include\%SDKV%"
set "LIBD=%SDKROOT%\Lib\%SDKV%"

set "INCLUDE=%MSVC%\include;%INCD%\ucrt;%INCD%\um;%INCD%\shared;%INCD%\winrt"
set "LIB=%MSVC%\lib\x64;%LIBD%\um\x64;%LIBD%\ucrt\x64"
set "PATH=%MSVC%\bin\Hostx64\x64;%PATH%"

cd /d "%~dp0"
cl /nologo /std:c++17 /utf-8 /EHsc /O2 /W3 /DUNICODE /D_UNICODE main.cpp src\*.cpp /Fe:InfiniteDesktop.exe /link /SUBSYSTEM:WINDOWS
if errorlevel 1 (
    echo [!] Build failed
    exit /b 1
)
echo [+] Done: InfiniteDesktop.exe

@echo off
REM Build InfiniteDesktop via MSVC Build Tools.
set "VS=C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools"
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 (
    echo [!] Failed to load MSVC environment
    exit /b 1
)
cd /d "%~dp0"
cl /nologo /std:c++17 /utf-8 /EHsc /O2 /W3 /DUNICODE /D_UNICODE main.cpp src\*.cpp /Fe:InfiniteDesktop.exe /link /SUBSYSTEM:WINDOWS
if errorlevel 1 (
    echo [!] Build failed
    exit /b 1
)
echo [+] Done: InfiniteDesktop.exe

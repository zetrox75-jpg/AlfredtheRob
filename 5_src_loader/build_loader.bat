@echo off
setlocal
rem Build FFLoader.exe (public, key-free launcher).
rem Output: FFLoader.exe in this folder. Pair it with FFLoader.ini.
cd /d "%~dp0"

set "VS="
for %%P in (
    "C:\Program Files\Microsoft Visual Studio\18\Community"
    "C:\Program Files\Microsoft Visual Studio\18\Professional"
    "C:\Program Files\Microsoft Visual Studio\2022\Community"
    "C:\Program Files\Microsoft Visual Studio\2022\Professional"
    "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools"
    "C:\Program Files\Microsoft Visual Studio\18\BuildTools"
    "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools"
    "C:\Program Files\Microsoft Visual Studio\2022\BuildTools"
) do if not defined VS if exist "%%~P\VC\Auxiliary\Build\vcvars64.bat" set "VS=%%~P"
if not defined VS (
    echo [ERROR] vcvars64.bat not found - edit the VS detection above.
    pause
    exit /b 1
)
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 ( echo VCVARS_FAIL & exit /b 1 )

set "CFLAGS=/nologo /O2 /W3 /MT /std:c++17 /D_CRT_SECURE_NO_WARNINGS /EHsc"
set "LIBS=shell32.lib gdiplus.lib ole32.lib comctl32.lib user32.lib gdi32.lib"

echo [1/2] compile
cl %CFLAGS% /c loader.cpp /Fo:loader.obj || goto fail
echo [2/2] link + manifest
rc /nologo /fo loader.res loader.rc || goto fail
cl %CFLAGS% loader.obj loader.res /Fe:FFLoader.exe /link /SUBSYSTEM:WINDOWS %LIBS% || goto fail

del /q loader.obj loader.res 2>nul
echo BUILD_OK  -  FFLoader.exe
exit /b 0

:fail
echo BUILD_FAIL
exit /b 1




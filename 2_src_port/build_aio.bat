@echo off
setlocal
rem Build the all-in-one launcher (RobRunAIO.exe).
rem
rem The launcher embeds the payload files from ..\1_run:
rem   kd2.exe, kernel-mode.sys, RobRunPort.exe, WebView2Loader.dll
rem so rebuild those first if you changed the overlay or the driver.
rem Output: RobRunAIO.exe here, copied to ..\1_run\RobRunAIO.exe.
cd /d "%~dp0"

set "VS="
for %%P in (
    "C:\Program Files\Microsoft Visual Studio\18\Community"
    "C:\Program Files\Microsoft Visual Studio\18\Professional"
    "C:\Program Files\Microsoft Visual Studio\2022\Community"
    "C:\Program Files\Microsoft Visual Studio\2022\Professional"
    "C:\Program Files\Microsoft Visual Studio\2022\Enterprise"
    "C:\Program Files\Microsoft Visual Studio\2022\BuildTools"
    "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools"
    "C:\Program Files\Microsoft Visual Studio\18\BuildTools"
    "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools"
) do if not defined VS if exist "%%~P\VC\Auxiliary\Build\vcvars64.bat" set "VS=%%~P"
if not defined VS (
    echo [ERROR] vcvars64.bat not found - install the MSVC C++ workload
    echo         or add your Visual Studio path to the detection list.
    pause
    exit /b 1
)
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 ( echo VCVARS_FAIL & exit /b 1 )

rem Payload inputs must exist before the resource compiler runs.
for %%F in ("..\1_run\kd2.exe" "..\1_run\kernel-mode.sys" "..\1_run\RobRunPort.exe" "..\1_run\WebView2Loader.dll") do (
    if not exist %%F (
        echo [ERROR] missing payload: %%~F
        pause
        exit /b 1
    )
)

set "CFLAGS=/nologo /O2 /W3 /MT /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS"

echo [1/3] resources
rc /nologo /fo aio.res aio_launcher.rc || goto fail
echo [2/3] compile
cl %CFLAGS% /c aio_launcher.c /Fo:aio_launcher.obj || goto fail
echo [3/3] link
link /nologo /SUBSYSTEM:WINDOWS /MACHINE:X64 /OUT:RobRunAIO.exe ^
  /MANIFEST:EMBED /MANIFESTINPUT:aio.manifest ^
  /MANIFESTUAC:"level='requireAdministrator' uiAccess='false'" ^
  aio_launcher.obj aio.res ^
  user32.lib gdi32.lib advapi32.lib comctl32.lib bcrypt.lib || goto fail

copy /Y RobRunAIO.exe "..\1_run\RobRunAIO.exe" >nul
echo.
echo BUILD_OK  -^>  RobRunAIO.exe  (copied to ..\1_run)
echo Run ..\1_run\RobRunAIO.exe as administrator; it prompts for elevation.
exit /b 0

:fail
echo BUILD_FAIL
exit /b 1

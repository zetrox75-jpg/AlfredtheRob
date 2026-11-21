@echo off
rem Live offset probe: dumps the current Il2CppClass field tables and the four
rem static-field RVAs for the running game. Requires the WmPort driver to be
rem mapped (MAP-DRIVER.bat once per boot) and ROB RUN.exe to be running.
cd /d "%~dp0"
if not exist "RobRunPort.exe" ( echo [STOP] RobRunPort.exe missing & pause & exit /b 1 )
echo Starting the offsets probe. It writes offsets.log next to this script.
echo If the driver is not mapped, run MAP-DRIVER.bat first.
start "" "RobRunPort.exe" --offsets
exit /b 0

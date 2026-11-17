@echo off
setlocal
set "pidPython=%PID_WORKBENCH_PYTHON%"
if not defined pidPython if exist "%~dp0runtime\python.exe" set "pidPython=%~dp0runtime\python.exe"
if not defined pidPython set "pidPython=%USERPROFILE%\.platformio\penv\Scripts\python.exe"
"%pidPython%" "%~dp0backend\pidctl.py" %*
exit /b %errorlevel%

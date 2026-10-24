@echo off
if defined PID_WORKBENCH_PYTHON (
  "%PID_WORKBENCH_PYTHON%" "%~dp0backend\pidctl.py" %*
) else (
  "%USERPROFILE%\.platformio\penv\Scripts\python.exe" "%~dp0backend\pidctl.py" %*
)
exit /b %errorlevel%

@echo off
where /q python
if not errorlevel 1 (
  python "%~dp0..\client_sdk.py" %*
) else (
  py "%~dp0..\client_sdk.py" %*
)
exit /b %errorlevel%

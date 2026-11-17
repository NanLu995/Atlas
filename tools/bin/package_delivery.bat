@echo off
where /q python
if not errorlevel 1 (
  python "%~dp0..\package_delivery.py" %*
) else (
  py "%~dp0..\package_delivery.py" %*
)
exit /b %errorlevel%

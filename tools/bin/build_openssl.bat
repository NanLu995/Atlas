@echo off
where /q python
if not errorlevel 1 (
  python "%~dp0..\build_openssl.py" %*
) else (
  py "%~dp0..\build_openssl.py" %*
)
exit /b %errorlevel%

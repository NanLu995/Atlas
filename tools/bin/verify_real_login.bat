@echo off
where /q python && (set "PY=python") || (set "PY=py")
%PY% "%~dp0..\verify_real_login.py" %*
exit /b %errorlevel%

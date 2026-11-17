@echo off
where /q python && (set "PY=python") || (set "PY=py")
%PY% "%~dp0..\database_recovery.py" %*
exit /b %errorlevel%

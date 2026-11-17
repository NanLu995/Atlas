@echo off
where /q python && (set "PY=python") || (set "PY=py")
%PY% "%~dp0..\prepare_mysql.py" %*
exit /b %errorlevel%

@echo off
where /q python && (set "PY=python") || (set "PY=py")
%PY% "%~dp0..\test_mysql.py" %*
exit /b %errorlevel%

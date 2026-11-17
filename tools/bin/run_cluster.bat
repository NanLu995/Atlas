@echo off
where /q python
if not errorlevel 1 (
  python "%~dp0..\run_cluster.py" %*
) else (
  py "%~dp0..\run_cluster.py" %*
)
exit /b %errorlevel%

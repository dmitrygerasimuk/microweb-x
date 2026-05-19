@echo off
setlocal
pushd "%~dp0"
wcl386 -q -bt=nt -l=nt -i=..\src -fe=microweb_tests.exe TestMain.cpp
set RESULT=%ERRORLEVEL%
popd
exit /b %RESULT%

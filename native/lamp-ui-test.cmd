@echo off
setlocal
set "ROOT=%~dp0.."
for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSROOT=%%i"
if not defined VSROOT exit /b 1
call "%VSROOT%\VC\Auxiliary\Build\vcvars64.bat" >nul
if not exist "%ROOT%\build\native" mkdir "%ROOT%\build\native"
pushd "%ROOT%\build\native"
cl /nologo /TC /O1 /MT /DNDEBUG /c "%ROOT%\native\vendor\zstddeclib.c" /Fo:lamp-zstd.obj
if errorlevel 1 (popd & exit /b 1)
cl /nologo /std:c++17 /utf-8 /O1 /MT /EHsc /W4 /DUNICODE /D_UNICODE "%ROOT%\native\core.cpp" "%ROOT%\native\lamp-ui-tests.cpp" lamp-zstd.obj /Fe:LampUiTests.exe /link /SUBSYSTEM:CONSOLE
if errorlevel 1 (popd & exit /b 1)
LampUiTests.exe
set "RESULT=%ERRORLEVEL%"
popd
exit /b %RESULT%

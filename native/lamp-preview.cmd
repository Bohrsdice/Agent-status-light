@echo off
setlocal
set "ROOT=%~dp0.."
for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSROOT=%%i"
if not defined VSROOT exit /b 1
call "%VSROOT%\VC\Auxiliary\Build\vcvars64.bat" >nul
if not exist "%ROOT%\build\native" mkdir "%ROOT%\build\native"
pushd "%ROOT%\build\native"
cl /nologo /std:c++17 /utf-8 /O1 /MT /EHsc /W4 /DUNICODE /D_UNICODE "%ROOT%\native\lamp-preview.cpp" /Fe:LampPreview.exe
if errorlevel 1 (popd & exit /b 1)
LampPreview.exe
set "RESULT=%ERRORLEVEL%"
popd
exit /b %RESULT%

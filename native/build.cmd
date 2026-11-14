@echo off
setlocal
set "ROOT=%~dp0.."
for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSROOT=%%i"
if not defined VSROOT exit /b 1
call "%VSROOT%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
if not exist "%ROOT%\build\native" mkdir "%ROOT%\build\native"
if not exist "%ROOT%\dist" mkdir "%ROOT%\dist"
pushd "%ROOT%\build\native"
rc /nologo /I "%ROOT%\native" /fo resources.res "%ROOT%\native\resources.rc"
if errorlevel 1 (popd & exit /b 1)
cl /nologo /TC /O1 /GL /Gy /Gw /MT /DNDEBUG /c "%ROOT%\native\vendor\zstddeclib.c" /Fo:zstddeclib.obj
if errorlevel 1 (popd & exit /b 1)
cl /nologo /std:c++17 /utf-8 /O1 /GL /Gy /Gw /MT /EHsc /W4 /DNDEBUG /DUNICODE /D_UNICODE "%ROOT%\native\core.cpp" "%ROOT%\native\main.cpp" zstddeclib.obj resources.res /Fe:"%ROOT%\dist\CodexLight.exe" /link /LTCG /OPT:REF /OPT:ICF /SUBSYSTEM:WINDOWS /DYNAMICBASE /NXCOMPAT /MANIFEST:EMBED /MANIFESTINPUT:"%ROOT%\native\app.manifest"
set "RESULT=%ERRORLEVEL%"
popd
exit /b %RESULT%

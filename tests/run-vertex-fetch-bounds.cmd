@echo off
setlocal
if defined VCToolsInstallDir goto compile
for /f "usebackq tokens=*" %%I in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "BF2_VSROOT=%%I"
if not defined BF2_VSROOT exit /b 1
call "%BF2_VSROOT%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
:compile
set "BF2_FETCH_TEST=%TEMP%\bf2-fetch-bounds-%RANDOM%"
cl /nologo /std:c++20 /EHsc /O2 "%~dp0vertex_fetch_bounds_test.cpp" "%~dp0..\project\src\native_gpu\xenos_hlsl.cpp" /Fe:"%BF2_FETCH_TEST%.exe" /Fo:"%TEMP%\\" /link d3d11.lib d3dcompiler.lib
if errorlevel 1 exit /b 1
"%BF2_FETCH_TEST%.exe"
exit /b %ERRORLEVEL%

@echo off
rem Convenience wrapper: finds the MSVC toolchain, then runs Maek.
rem Equivalent to running "node Maekfile.js" from an
rem "x64 Native Tools Command Prompt for VS20xx". Any arguments are passed
rem through, so "build.bat -v" and "build.bat -q -j1" work as usual.
setlocal
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
	echo Could not find vswhere.exe -- is Visual Studio installed?
	exit /b 1
)
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSPATH=%%i"
if not defined VSPATH (
	echo Could not find a Visual Studio install with the C++ build tools.
	exit /b 1
)
call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d "%~dp0"
node Maekfile.js %*

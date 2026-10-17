@echo off
rem Builds the loader proxy (out\version.dll) with Visual Studio 2022 Build Tools (x64).
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
cd /d "%~dp0"
if not exist out mkdir out
cl /nologo /LD /O2 /MT /W3 version_proxy.c /Fo:out\ /Fe:out\version.dll /link /NOLOGO /DEF:version.def
exit /b %errorlevel%

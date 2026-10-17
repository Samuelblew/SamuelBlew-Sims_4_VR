@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
cd /d "%~dp0"
cl /nologo /O2 /EHsc /std:c++17 /utf-8 /W3 /I ..\openxr\include teste_menu.cpp /Fe:teste_menu.exe /link user32.lib gdi32.lib gdiplus.lib shell32.lib ole32.lib
exit /b %errorlevel%

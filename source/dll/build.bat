@echo off
rem Compila fpc_rig.dll (x64) com o Visual Studio 2022 Build Tools + MinHook embutido.
rem Antes: os shaders de upscaling (FSR 1.0 / CAS da AMD, pasta fsr) viram bytecode embutido (fsr\out\*.h).
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
cd /d "%~dp0"
if not exist out mkdir out
if not exist fsr\out mkdir fsr\out
set FXC=fxc.exe
where fxc.exe >nul 2>nul || set FXC="C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64\fxc.exe"
for %%E in (VSFull:vs_5_0 PSEasu:ps_5_0 PSRcas:ps_5_0 PSBilinear:ps_5_0) do (
  for /f "tokens=1,2 delims=:" %%A in ("%%E") do (
    %FXC% /nologo /O3 /T %%B /E %%A /Vn g_sh%%A /Fh fsr\out\sh_%%A.h fsr\upscale_fsr.hlsl >nul || exit /b 1
  )
)
%FXC% /nologo /O3 /T ps_5_0 /E PSCas /Vn g_shPSCas /Fh fsr\out\sh_PSCas.h fsr\upscale_cas.hlsl >nul || exit /b 1
cl /nologo /LD /O2 /MT /EHsc /std:c++17 /utf-8 /W3 ^
  /I minhook\include /I openxr\include ^
  fpc_rig.cpp ^
  minhook\src\buffer.c minhook\src\hook.c minhook\src\trampoline.c minhook\src\hde\hde64.c ^
  /Fo:out\ /Fe:out\fpc_rig.dll ^
  /link /NOLOGO user32.lib gdi32.lib gdiplus.lib shell32.lib ole32.lib psapi.lib
exit /b %errorlevel%

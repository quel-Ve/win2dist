@echo off
call "D:\Program Files\VSBuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
cd /d "D:\Garage\Software\ccproject\12window2clear\native\dwm_inject"

set "MINHOOK=..\..\local\DWMBlurGlass-master\DWMBlurGlassExt\minhook"
set "KITSDK=C:\Program Files (x86)\Windows Kits\10"
set "SDKVER=10.0.22621.0"
set "SDKINC=%KITSDK%\Include\%SDKVER%"
set "SDKLIB=%KITSDK%\Lib\%SDKVER%\um\x64"

cl.exe /nologo /LD /Fe:build\libfrosted_dwm.dll /EHsc /O2 /MT ^
  src\dllmain_msvc.cpp ^
  %MINHOOK%\buffer.cpp ^
  %MINHOOK%\hook.cpp ^
  %MINHOOK%\trampoline.cpp ^
  %MINHOOK%\hde\hde32.c ^
  %MINHOOK%\hde\hde64.c ^
  /I%MINHOOK% /I%MINHOOK%\hde ^
  /I"%SDKINC%\um" /I"%SDKINC%\shared" /I"%SDKINC%\ucrt" ^
  d2d1.lib user32.lib advapi32.lib ^
  /link /LIBPATH:"%SDKLIB%" /MACHINE:X64

if %ERRORLEVEL% equ 0 (
  echo [OK] libfrosted_dwm.dll built
) else (
  echo [FAIL] Error %ERRORLEVEL%
)

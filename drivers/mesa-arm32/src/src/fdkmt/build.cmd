@echo off
rem Build loi fd_kmt -> build\fdkmt\fd_kmt.lib + fdkmt_probe.exe (ARM32, clang-cl, SDK 16299).
rem d3dkmthk.h lay tu SDK 19041 (chi 3 header).
setlocal
set ROOT=%~dp0..\..
call "F:\Program Files\Microsoft Visual Studio\2017\Professional\VC\Auxiliary\Build\vcvarsall.bat" x64_arm 10.0.16299.0 >nul || exit /b 1
set PATH=%ProgramFiles%\LLVM\bin;%PATH%
set SRC=%~dp0
set OUT=%ROOT%\build\fdkmt
if not exist "%OUT%" mkdir "%OUT%"
set KMTSRC=%ProgramFiles(x86)%\Windows Kits\10\Include\10.0.19041.0\shared
set KMTINC=%TEMP%\fdkmt_inc
if not exist "%KMTINC%" mkdir "%KMTINC%"
copy /y "%KMTSRC%\d3dkmthk.h" "%KMTINC%\" >nul
copy /y "%KMTSRC%\d3dkmdt.h" "%KMTINC%\" >nul
copy /y "%KMTSRC%\d3dukmdt.h" "%KMTINC%\" >nul
set CFLAGS=--target=arm-pc-windows-msvc /nologo /O2 /MT /W3 -D_ARM_WINAPI_PARTITION_DESKTOP_SDK_AVAILABLE=1 -DWINVER=0x0A00 -D_WIN32_WINNT=0x0A00 -D_CRT_SECURE_NO_WARNINGS /I"%KMTINC%" /I"%SRC%." /I"%ROOT%\third_party\mesa\src\freedreno\drm\d3dkmt"
cd /d "%OUT%"
for %%f in (fd_kmt gpu_table kmd_qcom) do (
  clang-cl %CFLAGS% /c "%SRC%%%f.c" /Fo:%%f.obj || exit /b 1
)
llvm-lib /nologo /machine:arm /out:fd_kmt.lib fd_kmt.obj gpu_table.obj kmd_qcom.obj || exit /b 1
clang-cl %CFLAGS% "%SRC%probe\fdkmt_probe.c" fd_kmt.lib /Fe:fdkmt_probe.exe -fuse-ld=lld || exit /b 1
clang-cl %CFLAGS% "%SRC%probe\fdkmt_share.c" fd_kmt.lib d3d11.lib dxgi.lib gdi32.lib dxguid.lib /Fe:fdkmt_share.exe -fuse-ld=lld || exit /b 1
echo OK: %OUT%\fd_kmt.lib, %OUT%\fdkmt_probe.exe

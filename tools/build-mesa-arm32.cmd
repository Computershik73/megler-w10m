@echo off
rem Mesa's freedreno for Windows 10 Mobile (ARM32) -- libEGL.dll, libGLESv2.dll and libgallium_wgl.dll -- built from
rem drivers\mesa-arm32\src: OpenGLES-For-Windows-Phone's D3DKMT core (src\src\fdkmt) and its 24 patches to Mesa 26.2.7
rem (src\tools\mesa-build), the way that project's own scripts build them (src\tools\mesa-build\build.cmd, which calls
rem src\src\fdkmt\build.cmd), with this PC's places for the tools.
rem
rem   tools\build-mesa-arm32.cmd            the three DLLs into <work>\out
rem   tools\build-mesa-arm32.cmd install    and into drivers\mesa-arm32, in place of the repository's
rem
rem 1. Mesa: its tag mesa-26.2.7 cloned into <work>\third_party\mesa once, and the patches applied with git am as their
rem    author committed them: the result is that project's own commit e963b78b, checked by its tree (else no build) and
rem    by its hash. Then Megler's own patches, megler-*.patch beside them (megler-0025: Mesa's crash handlers and its log
rem    files only when asked for; megler-0026: a failed BO allocation is out of memory, not a crash), and the tree they
rem    make checked. Mesa names the commit it is built from in its version
rem    string: "Mesa 26.2.7 (git-<the last patch's commit>)".
rem 2. The core: fd_kmt.c, gpu_table.c and kmd_qcom.c into <work>\build\fdkmt\fd_kmt.lib with clang-cl. Its D3DKMT
rem    declarations are SDK 10.0.19041's d3dkmthk.h and the two headers that includes, copied out alone (SDK 10.0.16299,
rem    which the rest is built with, has none for an app).
rem 3. Mesa in <work>\build\mesa-arm32 with meson and ninja: clang-cl for ARM32 (Thumb-2, arm32-clang-cl.ini) over VS
rem    2017's MSVC 14.16 headers and C runtime, linked in (/MT), and SDK 10.0.16299; only the three DLLs are built.
rem <work> has that project's layout (third_party\mesa beside build\mesa-arm32), so the source paths that Mesa's checks
rem write into the DLLs read as in its own builds (../../third_party/mesa/src/...).
rem
rem Environment (this PC's places by default):
rem   MEGLER_VS2017     Visual Studio 2017 with its ARM tools, MSVC 14.16 (...\2017\Enterprise, else ...\2017\BuildTools)
rem   MEGLER_LLVM       clang-cl, lld-link, llvm-lib and llvm-rc of LLVM 23: LLVM 22.1.8's ARM back end stops on Mesa's
rem                     C++ ("Cannot select: ... cleanupret" in texcompress_astc_luts.cpp). By default <work>\llvm\bin,
rem                     made of links to the LLVM 23.1.3 of llvm-mingw (MEGLER_LLVM_MINGW,
rem                     C:\tools\llvm-mingw-20261006-msvcrt-x86_64), which names its tools otherwise: clang-23.exe as
rem                     clang-cl.exe, ld.lld.exe as lld-link.exe (each takes its part from the name it is run by), the
rem                     DLLs they need beside them and a copy of lib\clang\23\include, the compiler's own headers. Its
rem                     LLVM prefers / in the paths it makes, LLVM's own Windows builds \: a header's path in the DLLs'
rem                     messages reads "src/util/simple_mtx.h" where theirs reads "src\util/simple_mtx.h"
rem   MEGLER_KMT_INC    d3dkmthk.h, d3dkmdt.h, d3dukmdt.h (...\Windows Kits\10\Include\10.0.19041.0\shared)
rem   MEGLER_MESON      the folder of meson.exe and ninja.exe, put before PATH, in the Python that has mako, pyyaml and
rem                     packaging for Mesa's generators (<work>\venv\Scripts: made with MEGLER_PYTHON, python by default,
rem                     and the versions below from PyPI when it is not there; meson 1.12.0 cannot do this cross build:
rem                     its check of the compiler puts the cross file's /machine:arm before /link)
rem   MEGLER_FLEXBISON  win_flex_bison 2.5.25 (<work>\tools\win_flex_bison-2.5.25: its GitHub release's zip is
rem                     downloaded and checked by SHA-256 when it is not there)
rem   MEGLER_MESA_WORK  the work folder, about 1.4 GB with the clone (build\mesa in the repository)
rem   MEGLER_JOBS       compilers at once (4)
rem The build folder is made once; delete <work>\build\mesa-arm32 after changing the tools or the options below.
rem tools\build-appx.sh takes the DLLs from <work>\build\mesa-arm32 itself with MEGLER_MESA set to that folder.

setlocal EnableExtensions
set "ROOT=%~dp0.."
for %%i in ("%ROOT%") do set "ROOT=%%~fi"
set "SRC=%ROOT%\drivers\mesa-arm32\src"
set "PATCHES=%SRC%\tools\mesa-build"

rem Mesa's tag and its commit; the commit and the tree the patches make of it (new patches: new values here).
set MESA_TAG=mesa-26.2.7
set MESA_BASE=b69adc2adfbfe3eb985bd66cd06c61ca719ae9ae
set MESA_HEAD=e963b78bede1a54e3abed2c30c618b8113b5b80c
set MESA_TREE=d31fe7a7cf4999b33097389b0b343cac20d48ac1
set MEGLER_TREE=e57a3ee32392961c07527852507d130a26055849
set FLEXBISON_URL=https://github.com/lexxmark/winflexbison/releases/download/v2.5.25/win_flex_bison-2.5.25.zip
set FLEXBISON_SHA256=8d324b62be33604b2c45ad1dd34ab93d722534448f55a16ca7292de32b6ac135
set PYTHON_MODULES=meson==1.12.1 ninja==1.13.2 mako==1.4.3 pyyaml==6.0.3 packaging==26.1

if not defined MEGLER_MESA_WORK set "MEGLER_MESA_WORK=%ROOT%\build\mesa"
set "WORK=%MEGLER_MESA_WORK%"
if not defined MEGLER_JOBS set MEGLER_JOBS=4
if not defined MEGLER_LLVM_MINGW set "MEGLER_LLVM_MINGW=C:\tools\llvm-mingw-20261006-msvcrt-x86_64"
if not defined MEGLER_KMT_INC set "MEGLER_KMT_INC=%ProgramFiles(x86)%\Windows Kits\10\Include\10.0.19041.0\shared"
if not defined MEGLER_FLEXBISON set "MEGLER_FLEXBISON=%WORK%\tools\win_flex_bison-2.5.25"
if not defined MEGLER_PYTHON set MEGLER_PYTHON=python
if not defined MEGLER_VS2017 if exist "%ProgramFiles(x86)%\Microsoft Visual Studio\2017\Enterprise\VC\Auxiliary\Build\vcvarsall.bat" set "MEGLER_VS2017=%ProgramFiles(x86)%\Microsoft Visual Studio\2017\Enterprise"
if not defined MEGLER_VS2017 set "MEGLER_VS2017=%ProgramFiles(x86)%\Microsoft Visual Studio\2017\BuildTools"
set "OUT=%WORK%\out"
if /i "%~1"=="install" set "OUT=%ROOT%\drivers\mesa-arm32"

set "MESA=%WORK%\third_party\mesa"
set "KMT=%WORK%\build\fdkmt"
set "BUILD=%WORK%\build\mesa-arm32"
if not exist "%PATCHES%\arm32-clang-cl.ini" echo build-mesa-arm32: no sources in %SRC% & exit /b 1
if not exist "%MEGLER_VS2017%\VC\Auxiliary\Build\vcvarsall.bat" echo build-mesa-arm32: no Visual Studio 2017 in %MEGLER_VS2017% [MEGLER_VS2017] & exit /b 1
if not exist "%MEGLER_KMT_INC%\d3dkmthk.h" echo build-mesa-arm32: no d3dkmthk.h in %MEGLER_KMT_INC% [MEGLER_KMT_INC] & exit /b 1
if not exist "%WORK%\dl" mkdir "%WORK%\dl" || exit /b 1
if not exist "%KMT%\inc" mkdir "%KMT%\inc" || exit /b 1
if not exist "%OUT%" mkdir "%OUT%" || exit /b 1

rem win_flex_bison: the release's zip, by its SHA-256, unpacked into a folder of its own.
if exist "%MEGLER_FLEXBISON%\win_flex.exe" goto have_flexbison
echo === win_flex_bison 2.5.25: %FLEXBISON_URL%
"%SystemRoot%\System32\curl.exe" -sSfL -o "%WORK%\dl\win_flex_bison-2.5.25.zip" "%FLEXBISON_URL%" || exit /b 1
set ZIPSUM=
for /f "skip=1 delims=" %%h in ('certutil -hashfile "%WORK%\dl\win_flex_bison-2.5.25.zip" SHA256') do if not defined ZIPSUM set "ZIPSUM=%%h"
set "ZIPSUM=%ZIPSUM: =%"
if /i not "%ZIPSUM%"=="%FLEXBISON_SHA256%" echo build-mesa-arm32: win_flex_bison-2.5.25.zip has SHA-256 %ZIPSUM%, not %FLEXBISON_SHA256% & exit /b 1
mkdir "%MEGLER_FLEXBISON%" || exit /b 1
"%SystemRoot%\System32\tar.exe" -xf "%WORK%\dl\win_flex_bison-2.5.25.zip" -C "%MEGLER_FLEXBISON%" || exit /b 1
:have_flexbison

rem meson, ninja and Mesa's Python modules, at the versions this was built with, in a Python of their own.
if defined MEGLER_MESON goto have_meson
set "MEGLER_MESON=%WORK%\venv\Scripts"
if exist "%MEGLER_MESON%\meson.exe" goto have_meson
echo === %PYTHON_MODULES%: %WORK%\venv
"%MEGLER_PYTHON%" -m venv "%WORK%\venv" || exit /b 1
"%WORK%\venv\Scripts\python.exe" -m pip install -q --disable-pip-version-check %PYTHON_MODULES% || exit /b 1
:have_meson

rem LLVM 23: llvm-mingw's, under the names meson and Mesa's cross file ask for (hard links: same volume, else copies).
if defined MEGLER_LLVM goto have_llvm
set "MEGLER_LLVM=%WORK%\llvm\bin"
if exist "%MEGLER_LLVM%\clang-cl.exe" goto have_llvm
if not exist "%MEGLER_LLVM_MINGW%\bin\clang-23.exe" echo build-mesa-arm32: no LLVM 23 [MEGLER_LLVM, MEGLER_LLVM_MINGW] & exit /b 1
echo === LLVM 23 of %MEGLER_LLVM_MINGW%: %MEGLER_LLVM%
mkdir "%WORK%\llvm\bin" || exit /b 1
xcopy "%MEGLER_LLVM_MINGW%\lib\clang\23\include" "%WORK%\llvm\lib\clang\23\include\" /e /i /q /y >nul || exit /b 1
mklink /H "%MEGLER_LLVM%\clang-cl.exe" "%MEGLER_LLVM_MINGW%\bin\clang-23.exe" >nul || copy /y "%MEGLER_LLVM_MINGW%\bin\clang-23.exe" "%MEGLER_LLVM%\clang-cl.exe" >nul || exit /b 1
mklink /H "%MEGLER_LLVM%\lld-link.exe" "%MEGLER_LLVM_MINGW%\bin\ld.lld.exe" >nul || copy /y "%MEGLER_LLVM_MINGW%\bin\ld.lld.exe" "%MEGLER_LLVM%\lld-link.exe" >nul || exit /b 1
for %%f in (llvm-lib.exe llvm-rc.exe libLLVM-23.dll libclang-cpp.dll libc++.dll libunwind.dll) do mklink /H "%MEGLER_LLVM%\%%f" "%MEGLER_LLVM_MINGW%\bin\%%f" >nul || copy /y "%MEGLER_LLVM_MINGW%\bin\%%f" "%MEGLER_LLVM%\" >nul || exit /b 1
:have_llvm
if not exist "%MEGLER_LLVM%\clang-cl.exe" echo build-mesa-arm32: no clang-cl.exe in %MEGLER_LLVM% [MEGLER_LLVM] & exit /b 1

rem Mesa, and the patches as their author committed them: the committer is the author, at the author's time, but for
rem the last patch, which its author committed again 20 seconds after (its commit's date in e963b78b). Then Megler's own
rem patches (megler-*.patch), each committed at its author's time by the same committer, so that the commit the DLLs'
rem version string names is the same on every build.
if exist "%MESA%\.git" goto have_mesa
echo === Mesa %MESA_TAG% into %MESA%
git clone -c core.autocrlf=false --depth 1 -b %MESA_TAG% https://gitlab.freedesktop.org/mesa/mesa.git "%MESA%" || exit /b 1
:have_mesa
set TREE=
for /f %%h in ('git -C "%MESA%" rev-parse HEAD:') do set "TREE=%%h"
if "%TREE%"=="%MEGLER_TREE%" goto patched
echo === The patches, onto %MESA_BASE%
git -C "%MESA%" am --abort >nul 2>&1
git -C "%MESA%" checkout -q -f -B wp-arm32 %MESA_BASE% || exit /b 1
copy /b "%PATCHES%\0*.patch" "%WORK%\dl\patches.mbox" >nul || exit /b 1
git -C "%MESA%" -c user.name=anon -c user.email= am -q --committer-date-is-author-date "%WORK%\dl\patches.mbox" || exit /b 1
set "GIT_COMMITTER_DATE=1795225589 +0700"
git -C "%MESA%" -c user.name=anon -c user.email= commit -q --amend --no-edit --no-verify || exit /b 1
set GIT_COMMITTER_DATE=
for /f %%h in ('git -C "%MESA%" rev-parse HEAD:') do set "TREE=%%h"
if not "%TREE%"=="%MESA_TREE%" echo build-mesa-arm32: Mesa's tree is %TREE% with the project's patches, not %MESA_TREE% & exit /b 1
for /f %%h in ('git -C "%MESA%" rev-parse HEAD') do set "HEAD=%%h"
if not "%HEAD%"=="%MESA_HEAD%" echo build-mesa-arm32: WARNING: the tree is right, but the project's commit came out as %HEAD%, not %MESA_HEAD%
echo === Megler's patches
copy /b "%PATCHES%\megler-*.patch" "%WORK%\dl\megler.mbox" >nul || exit /b 1
git -C "%MESA%" -c user.name=anon -c user.email= am -q --committer-date-is-author-date "%WORK%\dl\megler.mbox" || exit /b 1
:patched
for /f %%h in ('git -C "%MESA%" rev-parse HEAD:') do set "TREE=%%h"
if not "%TREE%"=="%MEGLER_TREE%" echo build-mesa-arm32: Mesa's tree is %TREE% with Megler's patches, not %MEGLER_TREE% & exit /b 1
for /f %%h in ('git -C "%MESA%" rev-parse HEAD') do set "HEAD=%%h"
echo === Mesa at %HEAD%: the project's %MESA_HEAD% and Megler's patches

rem VS 2017's ARM tools (MSVC 14.16) and SDK 10.0.16299: INCLUDE and LIB for clang-cl and lld-link.
call "%MEGLER_VS2017%\VC\Auxiliary\Build\vcvarsall.bat" x64_arm 10.0.16299.0 -vcvars_ver=14.16 >nul || exit /b 1
set "PATH=%MEGLER_MESON%;%MEGLER_LLVM%;%MEGLER_FLEXBISON%;%PATH%"
if not exist "%MEGLER_MESON%\meson.exe" echo build-mesa-arm32: no meson.exe in %MEGLER_MESON% [MEGLER_MESON] & exit /b 1

echo === The D3DKMT core: %KMT%\fd_kmt.lib
for %%h in (d3dkmthk.h d3dkmdt.h d3dukmdt.h) do copy /y "%MEGLER_KMT_INC%\%%h" "%KMT%\inc\" >nul || exit /b 1
set CFLAGS=--target=arm-pc-windows-msvc /nologo /O2 /MT /W3 -D_ARM_WINAPI_PARTITION_DESKTOP_SDK_AVAILABLE=1 -DWINVER=0x0A00 -D_WIN32_WINNT=0x0A00 -D_CRT_SECURE_NO_WARNINGS
for %%f in (fd_kmt gpu_table kmd_qcom) do clang-cl %CFLAGS% /I"%KMT%\inc" /I"%SRC%\src\fdkmt" /I"%MESA%\src\freedreno\drm\d3dkmt" /c "%SRC%\src\fdkmt\%%f.c" /Fo"%KMT%\%%f.obj" || exit /b 1
llvm-lib /nologo /machine:arm /out:"%KMT%\fd_kmt.lib" "%KMT%\fd_kmt.obj" "%KMT%\gpu_table.obj" "%KMT%\kmd_qcom.obj" || exit /b 1

echo === Mesa: %BUILD%
set "FDKMT_LIB=%KMT:\=/%/fd_kmt.lib"
if exist "%BUILD%\build.ninja" goto configured
"%MEGLER_MESON%\meson.exe" setup "%BUILD%" "%MESA%" --cross-file "%PATCHES%\arm32-clang-cl.ini" --buildtype=debugoptimized -Db_vscrt=mt --wrap-mode=nofallback -Dplatforms=windows -Dwgl-corewindow=true -Dgallium-drivers=freedreno -Dvulkan-drivers= -Dfreedreno-kmds=d3dkmt -Dopengl=true -Dgles1=disabled -Dgles2=enabled -Degl=enabled -Dglx=disabled -Dgbm=disabled -Dllvm=disabled -Dshared-glapi=enabled -Dzlib=disabled -Dzstd=disabled -Dexpat=disabled -Dvalgrind=disabled -Dlibunwind=disabled -Dbuild-tests=false -Dfreedreno-d3dkmt-stub=false "-Dfreedreno-d3dkmt-lib=%FDKMT_LIB%" || exit /b 1
:configured
rem (ninja does not follow fd_kmt.lib, a link argument: the driver is linked again each time.)
del /q "%BUILD%\src\gallium\targets\wgl\libgallium_wgl.dll" 2>nul
"%MEGLER_MESON%\ninja.exe" -C "%BUILD%" -j %MEGLER_JOBS% src/gallium/targets/wgl/libgallium_wgl.dll src/egl/libEGL.dll src/mesa/glapi/es2api/libGLESv2.dll || exit /b 1

echo === Into %OUT%
copy /y "%BUILD%\src\egl\libEGL.dll" "%OUT%\" >nul || exit /b 1
copy /y "%BUILD%\src\mesa\glapi\es2api\libGLESv2.dll" "%OUT%\" >nul || exit /b 1
copy /y "%BUILD%\src\gallium\targets\wgl\libgallium_wgl.dll" "%OUT%\" >nul || exit /b 1
for %%d in (libEGL.dll libGLESv2.dll libgallium_wgl.dll) do for /f "delims=" %%h in ('certutil -hashfile "%OUT%\%%d" SHA256 ^| findstr /v ":"') do echo %%h  %%d
exit /b 0

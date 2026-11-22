@echo off
rem Build Mesa freedreno (gallium + ir3 + EGL/GLES WGL CoreWindow) cho Windows 10 Mobile ARM32,
rem link voi loi D3DKMT src\fdkmt (build truoc).
rem Can: VS 2017 (MSVC 14.16 ARM), SDK 16299, LLVM (clang-cl, lld-link),
rem      tools\win_flex_bison-2.5.25, Python 3 + meson ninja mako pyyaml packaging.
rem   build.cmd [ninja target...]
rem   set FDKMT_STUB=1  -> link fd_kmt_stub.c thay vi loi that.
setlocal
set ROOT=%~dp0..\..
set VSDIR=F:\Program Files\Microsoft Visual Studio\2017\Professional

rem 1. Loi fd_kmt (build\fdkmt\fd_kmt.lib)
call "%ROOT%\src\fdkmt\build.cmd" >nul || (echo Loi build src\fdkmt & exit /b 1)
set FDKMT_LIB=%ROOT:\=/%/build/fdkmt/fd_kmt.lib
if "%FDKMT_STUB%"=="1" (set FDKMT_OPTS=-Dfreedreno-d3dkmt-stub=true) else (set FDKMT_OPTS=-Dfreedreno-d3dkmt-stub=false -Dfreedreno-d3dkmt-lib=%FDKMT_LIB%)

rem 2. Mesa
call "%VSDIR%\VC\Auxiliary\Build\vcvarsall.bat" x64_arm 10.0.16299.0 >nul || exit /b 1
set PATH=%APPDATA%\Python\Python311\Scripts;%ProgramFiles%\LLVM\bin;%~dp0..\win_flex_bison-2.5.25;%PATH%
set BUILD=%ROOT%\build\mesa-arm32
if not exist "%BUILD%\build.ninja" (
  meson setup "%BUILD%" "%ROOT%\third_party\mesa" --cross-file "%~dp0arm32-clang-cl.ini" ^
    --buildtype=debugoptimized -Db_vscrt=mt --wrap-mode=nofallback ^
    -Dplatforms=windows -Dwgl-corewindow=true -Dgallium-drivers=freedreno -Dvulkan-drivers= ^
    -Dfreedreno-kmds=d3dkmt -Dopengl=true -Dgles1=disabled -Dgles2=enabled ^
    -Degl=enabled -Dglx=disabled -Dgbm=disabled -Dllvm=disabled ^
    -Dshared-glapi=enabled -Dzlib=disabled -Dzstd=disabled -Dexpat=disabled ^
    -Dvalgrind=disabled -Dlibunwind=disabled -Dbuild-tests=false %FDKMT_OPTS% || exit /b 1
) else (
  meson configure "%BUILD%" %FDKMT_OPTS% >nul || exit /b 1
)
rem ninja khong theo doi .lib ngoai (link_args) -> ep link lai DLL driver
del /q "%BUILD%\src\gallium\targets\wgl\libgallium_wgl.dll" 2>nul
ninja -C "%BUILD%" %* || exit /b 1

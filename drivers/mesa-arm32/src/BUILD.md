# Build guide

## 1. PC requirements (Windows x64)
- **Visual Studio 2017** with the ARM toolset (MSVC 14.16) — VS 2019/2022 no longer ship ARM32. Use `vcvarsall x64_arm 10.0.16299.0`.
- **Windows SDK 10.0.16299** (+ WDK 1709). `d3dkmthk.h` and 2 related headers are taken from SDK 19041 (copy only those 3 headers, not the whole `shared` folder).
- **LLVM** (clang-cl, lld-link, llvm-lib) in `%ProgramFiles%\LLVM\bin`.
- **win_flex_bison 2.5.25** extracted to `tools\win_flex_bison-2.5.25\`.
- **Python 3** + `pip install --user meson ninja mako pyyaml packaging`.
- Adjust the VS/SDK paths in the `build.cmd` scripts if they are installed elsewhere on your machine.

## 2. Get Mesa and apply the patches
```
git clone --depth 1 -b mesa-26.2.7 https://gitlab.freedesktop.org/mesa/mesa.git third_party/mesa
cd third_party/mesa
git checkout -b wp-arm32
git am ../../tools/mesa-build/*.patch
cd ../..
```

## 3. Build
```
tools\mesa-build\build.cmd
```
The script first builds the core `src\fdkmt\build.cmd` → `build\fdkmt\fd_kmt.lib`, then Mesa (clang-cl, `-Dwgl-corewindow=true`)
into `build\mesa-arm32\`. Main outputs: `libgallium_wgl.dll`, `libEGL.dll`, `libGLESv2.dll`.
`set FDKMT_STUB=1` builds against the stub core (no device needed). Details + level-2 test (`fdtest.exe`): `tools/mesa-build/README.md`.

Test app:
```
tools\glesapp\build.cmd
signtool sign /fd SHA256 /f <your-cert>.pfx /p <password> tools\glesapp\out\glesapp.appx
```
(Create a test certificate with `New-SelfSignedCertificate`; its Publisher must match `AppxManifest.xml`.)

## 4. Install on the device
- The device needs: Developer mode, telnet/SSH (Interop Unlock), and the test certificate trusted on the device.
- `WinAppDeployCmd` often fails with `0x80070005` → copy the appx to the device and run via telnet:
  `DeployAppx.exe install force glesapp.appx`.
- Copy files: `python3 tools/phone/push.py <file>` (the device pulls them from the PC with `ftp.exe`); telnet: `python3 tools/phone/tn.py "command"`. Edit the device IP in the scripts.
- App log: `...\Packages\<PackageFamily>\AC\Temp\glesapp.log` (close the app before reading).
- **Keep the phone screen on** while working (screen off → Wi-Fi sleeps → connection lost).

## 5. Useful environment variables
| Variable | Effect |
|---|---|
| `FD_D3DKMT_GMEM=1` | Enable GMEM rendering (~4× faster when bandwidth-bound, but may hang — see ISSUES) |
| `FD_MESA_DEBUG=...` | Standard freedreno debug flags |
| `FD5_PREEMPT_REC=1` | Enable a5xx preemption save-record (experimental) |
| `FD_MAX_BATCH_DRAWS=N` | Limit draws per batch (debug) |

The UWP app also reads `%TEMP%\glesapp.env` (`KEY=VAL` lines) before initializing EGL.

## 6. dEQP
`tools/deqp-build/` builds dEQP for ARM32; run by group: `tools/phone/deqp_groups.py <gles2|gles3> [ENV=..] <group>...`,
summarize with `deqp_sum.py <log>`, extract failing images with `qpa_images.py <qpa> <dir>`. Always pass `--deqp-surface-width/height=256`.

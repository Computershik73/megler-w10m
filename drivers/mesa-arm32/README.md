# Mesa (freedreno over D3DKMT) for Windows 10 Mobile, ARM32

OpenGL ES and EGL for the phone's Adreno GPU (tested on the HP Elite x3, Adreno 530), beside ANGLE: Windows 10 Mobile's
package (`tools/build-appx.sh arm`, not `--rt`) carries both, and each app chooses at its start (Settings, **Graphics**;
`docs/ARCHITECTURE.md`, "Mesa's freedreno, chosen per app"). `MEGLER_MESA` names another folder of these DLLs;
`MEGLER_GLES=mesa` makes Mesa the choice where none is set, `MEGLER_GLES=angle` leaves Mesa out.

| File | In the package as | What |
|---|---|---|
| `libEGL.dll` | `libEGL_mesa.dll` | EGL 1.5; extensions include `EGL_ANGLE_surface_d3d_texture_2d_share_handle`, `EGL_ANGLE_d3d_share_handle_client_buffer`, `EGL_ANGLE_query_surface_pointer`, `EGL_KHR_fence_sync` |
| `libGLESv2.dll` | `libGLESv2_mesa.dll` | OpenGL ES 2/3 entry points |
| `libgallium_wgl.dll` | `libgallium_wgl.dll` | the driver: Mesa's gallium freedreno, its kernel interface over D3DKMT |

The first two are renamed in the package because ANGLE's `libEGL.dll` and `libGLESv2.dll` are at its root too: Windows takes
a DLL asked for by name for any module of that name already loaded, and ANGLE's `libEGL.dll` imports `libGLESv2.dll` by
name. Both of Mesa's import `libgallium_wgl.dll` alone (besides `KERNEL32.dll` and API sets), which Windows finds beside
them; nothing in them looks for their own names. An app's own package carries the three only while the app goes by Mesa
(`app/Megler/shell/Graphics.h`).

## Their source, and building them

Built here, from the source beside them in `src/`, by `tools\build-mesa-arm32.cmd` (`... install` puts them in this
folder; its head lists the tools and the variables that name them). `src/` is the OpenGLES-For-Windows-Phone project of
thaihoangduylinh, who sent the first build of these DLLs (pull request #34), as its author published it
(`OpenGLES_Snap820-WP.zip`), in its own layout:

- `src/src/fdkmt`: the D3DKMT core, through which Mesa's freedreno does everything it would ask a Linux kernel driver
  for. It finds the first hardware adapter, takes the SoC from the name of its user-mode driver (`qcdx11um8996.dll`:
  8996, an Adreno 530; `gpu_table.c`), and makes a device, a virtual 3D context, a paging queue and a monitored fence;
  buffers are allocations in the Qualcomm kernel driver's private-data format (`kmd_qcom.c`), made resident, mapped for
  the GPU and locked for the CPU; command buffers are submitted by their GPU address and waited for on the fence. It
  calls the D3DKMT functions of `gdi32.dll`, which it loads. Its `probe/` and `tools/mkalloc.py` are for bringing it to
  another GPU, not part of the DLLs.
- `src/tools/mesa-build`: 24 patches to Mesa 26.2.7 -- freedreno's `d3dkmt` backend over that core, a winsys and a
  WGL/EGL front end for a CoreWindow without GDI (`STW_COREWINDOW`), pbuffers drawn into Direct3D 11 shared textures
  with ANGLE's share-handle EGL extensions (what Megler draws through), fixes for the a5xx (guardband, stream-out,
  depth/stencil tiled in sysmem rendering, sysmem rendering by default), for `util_queue` and for EGL's display, and a
  backtrace on a crash -- with the meson cross file and the project's own build script.
- Mesa is not here: its tag `mesa-26.2.7` (commit `b69adc2a`), cloned by the script. The patches applied as their
  author committed them make exactly the project's Mesa commit `e963b78bede1a54e3abed2c30c618b8113b5b80c` (tree
  `d31fe7a7`), which the script checks; the DLLs name it ("Mesa 26.2.7 (git-e963b78bed)").

These were built with VS 2017 15.9's MSVC 14.16.27023 headers and C runtime, linked in (they import `KERNEL32.dll`,
API sets and `d3d11.dll` alone), SDK 10.0.16299 (and `d3dkmthk.h`, `d3dkmdt.h`, `d3dukmdt.h` of 10.0.19041), LLVM
23.1.3 (llvm-mingw 20261006's clang and lld, as `clang-cl` and `lld-link`), meson 1.12.1, ninja 1.13.2, Python 3.14 with
mako 1.4.3, PyYAML 6.0.3 and packaging 26.1, and win_flex_bison 2.5.25; ARM32 (Thumb-2) for Windows 10 Mobile 1709. LLVM
22.1.8 cannot build them, meson 1.12.0 cannot set the build up (the script says why). Built again (twice, in two work
folders, from nothing), they differ in the link time and the PDB's path and identity alone.

Pull request #34's DLLs, the project's own build, export and import the same. Every function of its `libEGL.dll` and
`libGLESv2.dll` is the same byte for byte but for the addresses in it, and so is every one of `libgallium_wgl.dll`'s
29110 but two in `freedreno_gmem.c`: #34's carried a test the published source does not have (`gmem_stateobj_init` read
`FD_NBINS`, `<x>x<y>`, to raise the number of GMEM bins; GMEM rendering is off by default) and so an assertion's line
number 10 lines later in `fd_gmem_render_tiles`. It was linked while its tree's commit was still the 23rd patch's, which
its version string named (`git-bf917323ed`). The rest differs only in what names the build: the PDB's path, the link
time, and the separator before the name of a header in the paths assertion messages carry (`src/compiler/nir\nir.h` in
theirs, made by LLVM's own Windows build; `src/compiler/nir/nir.h` in these), which also changes which equal strings the
linker merges.

## When they run

- The driver writes `%TEMP%\fdkmt.log` while it works: counts and times of its allocations, submits and waits, a line
  every 2 seconds. A pbuffer whose shared texture cannot be made is told to `%TEMP%\fd_egl_share.log`,
  `OutputDebugString` and stderr; the CoreWindow swap chain, which Megler does not use, times its presents into
  `%TEMP%\fd_wgl.log`. Environment variables (`src/BUILD.md`, `src/src/fdkmt/README.md`, the patches) switch on more:
  `FDKMT_DEBUG`, `FDKMT_DUMP_HANG`, `FDKMT_DUMP_FAIL`, `FD_D3DKMT_GMEM`, `FD_D3DKMT_CACHED`, `FD_MAX_BATCH_DRAWS`, ...
- Its first screen (EGL's first `eglInitialize` on it) installs, for the whole process and first in line, a vectored
  exception handler that writes a backtrace to its own stderr for every exception but a C++ throw, a debug print and a
  thread name, and passes it on (`wgl_screen_create`, patches 6 and 15); also its C runtime's handlers for an invalid
  parameter and SIGABRT. Megler's own handler, installed first in line earlier, runs after it from then on.

SHA-256:

```
2bb34bf884076c8e235ee4d2be331c07899d7b18bceedfd60ce55376fee6c4be  libEGL.dll
cf18646ec1714dd2da5e0b1bf87f3dddd31cad567314d5427ef186e9fc75fd53  libGLESv2.dll
006d353ce743393a8ec1cd6edbd2215921d01e19062643752d8c2b2daf32c0a2  libgallium_wgl.dll
```

Licences: `LICENSES/`, whose `README.md` says which code in the DLLs is under which. Mesa is MIT for nearly all of it
(Mesa's own word: `LICENSES/license.rst`, its `docs/license.rst`); Khronos' headers are Apache-2.0, C11 threads for
Win32 BSL-1.0, xxHash and SoftFloat BSD, and the texts are those of Mesa's `licenses/` folder or of the files themselves.
The patches' new files are MIT by their SPDX tags; the D3DKMT core (`src/src/fdkmt`) carries no licence notice.

How Megler uses it: `core/src/ndk/mg_egl.c` (`mesa_load`: Mesa taken where the app asks for it and its display
initializes, else ANGLE; the display, and Mesa's extension functions through its `eglGetProcAddress`),
`core/src/drivers/mg_java_gl.c` (window surfaces as shared textures in turn, with fences), `app/Megler/player/GfxHost.cpp`
(the shell composes them; Mesa's rows come top first, `megler_gles`).

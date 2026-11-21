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

Built from the OpenGLES-For-Windows-Phone project (its commit `22701276889704ebe718ba4864cbc84dcefc7a8a`,
2026-11-21; Mesa at `e963b78bede1a54e3abed2c30c618b8113b5b80c`, "egl/wgl, freedreno: every RGBA8 pbuffer gets a
D3D11 share handle"), ARM32, MSVC 14.16. The C runtime is linked in: they import `KERNEL32.dll`, API sets and
`d3d11.dll` alone. Targets Windows 10 Mobile 1709 (SDK 16299).

SHA-256:

```
4a8f85312b28d3b289644fa97214d95e0cc2b1ae7a45a4c9530b35978d0ace23  libEGL.dll
457510c2b7a878764dbd85ece5b18485f31ef756a1942d9ce9471878380f647d  libGLESv2.dll
6a2248fa9faafe92cbf930a9deaa8741cab23c918ae269143c793a268936d725  libgallium_wgl.dll
```

Mesa's licences are in `MESA-LICENSE.rst` (MIT for the most part; see that file for the others).

How Megler uses it: `core/src/ndk/mg_egl.c` (`mesa_load`: Mesa taken where the app asks for it and its display
initializes, else ANGLE; the display, and Mesa's extension functions through its `eglGetProcAddress`),
`core/src/drivers/mg_java_gl.c` (window surfaces as shared textures in turn, with fences), `app/Megler/player/GfxHost.cpp`
(the shell composes them; Mesa's rows come top first, `megler_gles`).

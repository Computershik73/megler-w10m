# Megler's changes to ANGLE

Megler's package uses gecko-w10m's ANGLE (`libEGL.dll`, `libGLESv2.dll`, built by
gecko-w10m as part of Firefox) with the changes here. Each is a patch against
ANGLE's tree as gecko-w10m has it (`engine/firefox/third_party/angle`), paths
relative to that tree. `tools/build-angle.py` applies them to copies of the
files they change, compiles again the objects of `libGLESv2.dll` that have
those files in them, with gecko-w10m's compiler and flags, and links
`libGLESv2.dll` into `build/angle/dist`; gecko-w10m's sources and its object
directory are left as they are. `tools/build-appx.sh` takes that ANGLE where it
is there. A change to a header is not followed into the objects that include
it: these patches change `.cpp` files only.

- `0002-buffer11-constant-buffer-ranges-reused.patch` -- `Buffer11.cpp`. Where
  the driver has no constant buffer offsets (Direct3D 11.1's
  `ConstantBufferOffsetting`), ANGLE makes a Direct3D buffer of its own for each
  range of a uniform buffer an app binds (`glBindBufferRange`), keeps them by
  offset, and deletes the least recently used once they come to twice the
  buffer's size. An app that binds the next range of one large uniform buffer for
  each draw had a buffer made, and one deleted, for nearly every draw: in a race
  of Asphalt 9's, two thirds of its render thread was under
  `ResourceManager11::allocate<ID3D11Buffer>` and the driver's calls into the
  kernel, at one or two frames a second with the graphics processor idle. Once
  the ranges' buffers come to that size, the least recently used one, when large
  enough, is taken for the new offset instead, and written again (with
  `WRITE_DISCARD`, so the draws already made with it keep what they read); one
  used by the last 64 ranges asked for is never taken.

- `0003-platform-helpers-app-container.patch` -- `platform_helpers.cpp`, for the
  ANGLE of desktop Windows (`tools/build-angle.py --arch`, which defines
  `ANGLE_MEGLER_APP_CONTAINER`; the phone's build does not, and its code stays as
  it was). gecko-w10m's build compiles ANGLE's common code as the desktop's, not
  as UWP's, so its Windows version check called `VerifyVersionInfoW`, which
  WindowsApp.lib takes from `api-ms-win-core-kernel32-legacy-l1-1-1.dll`: the one
  import of the DLLs that neither Megler.exe has nor Direct3D, and that an Xbox
  may not have for an app. The check gives UWP's answer instead (Windows 10 or
  later, which every Megler of desktop Windows runs on). Since 0.1.0.680 the
  desktop DLLs are linked as Megler.exe is, with WindowsApp.lib, and import
  api-sets, `d3d11.dll`, `dxgi.dll` and `d3dcompiler_47.dll` alone: an Xbox has no
  `kernel32.dll` for an app, and ANGLE linked with `kernel32.lib` loaded for no app
  there (0.1.0.679, "ANGLE did not load").

- `0004-gles1-feature-level-9.patch` -- `GLES1Renderer.cpp`, OpenGL ES 1 on a
  display of OpenGL ES 2 at most: Direct3D 11 at feature level 9_3, a Lumia
  650's Adreno 304 and most Lumias'. ANGLE draws version 1 itself, with an uber
  shader of ESSL 3.00 specialised by constants (uint loops and switches, flat
  varyings, array constructors, `gl_VertexID`), which such a display does not
  compile (at 9_3 the translator also holds it to ESSL 1.00's limits on
  indexing: "'vertexId' : Index expression can only contain const or loop
  symbols"). The first draw of version 1 was refused there, and the second ended
  the app: `initializeRendererProgram` had kept the failed state's entry with no
  program and asked it for its id (gl::Program::id on null -- Transit's Google
  Maps on a Lumia 650, 0.1.0.683; this PC's own ANGLE at 9_3 without the patch
  does the same at the second draw). Where the display makes OpenGL ES 2 at most
  (`Display::getMaxSupportedESVersion`), the shaders are ESSL 1.00 written for
  the state they draw in: every choice the uber shader makes from its constants
  -- lights, texture units and their environments and combiners, fog, alpha
  test, clip planes, point size, `glDrawTexOES` -- made in C++, the arithmetic
  the uber shader's line for line, so the pictures are the same at either level
  (`core/tests/mg_angle_windows.c`, on this PC's build of it: thirty scenes,
  every picture the same at 9_3 as at 12_1, pixel for pixel). ESSL 1.00 has no
  flat varyings (a flat shade model is drawn smooth) and 9_3 no framebuffer
  fetch (no logic op, as before), and no `gl_VertexID`: `glDrawTexOES` reads its
  rectangle's corners from the point size's attribute, a client array for that
  draw, the app's own given back after it. And on every display a state whose
  shaders do not compile or link is kept with no program: its draws are refused
  with `GL_INVALID_OPERATION` and its shaders and program let go of, never asked
  for an id; the shader manager is made once (it was made again, and leaked,
  after each failure). Megler's core finds out by drawing once whether the
  package's ANGLE has this (`ndk/mg_egl.c`, `es1_drawn_here`), and where it does
  not, makes version-1 contexts of version 2 there, as 0.1.0.684 did.

(An earlier `0001-buffer11-dynamic-writes.patch` took a static buffer written
after its draws as dynamic. It was made for a loading screen that ran at seven
frames a second, which turned out to be the shell composing the window from
frames read back, not ANGLE's buffers; with it and without it a race ran the
same, and it is gone.)

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

- `0001-buffer11-dynamic-writes.patch` -- `Buffer11.cpp`. A buffer made with a
  static usage (`GL_STATIC_DRAW`) that the app writes again after it has drawn
  from it is taken as dynamic, as ANGLE keeps a buffer made dynamic: its data in
  system memory, the parts a draw uses streamed to that draw. And a dynamic
  buffer is mapped (`glMapBufferRange`) in its system memory. Before, every
  write to such a buffer, and every mapping of any buffer, went to a staging
  copy of the whole buffer and from there, on the graphics processor, to the
  buffer the draws read; the next write waited for that copy. Asphalt 9, which
  streams its geometry, ran at one or two frames a second with a processor
  three quarters idle. `BufferD3D::promoteStaticUsage` makes the buffer static
  again once it is drawn from unchanged long enough.

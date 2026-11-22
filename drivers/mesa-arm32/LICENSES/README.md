# Licences of the code in Mesa's DLLs

What goes into `libEGL.dll`, `libGLESv2.dll` and `libgallium_wgl.dll` (every source and header their build compiled,
from ninja's own record of it, `ninja -t inputs` and `-t deps`, read for each file's SPDX tag or licence notice):

| Code | Licence | Text |
|---|---|---|
| Mesa: nearly all of it (EGL, the GL state tracker, gallium, freedreno, ir3, NIR, GLSL, util) | MIT | `MIT`; Mesa's own word on its licences: `license.rst` (its `docs/license.rst`) |
| The patches' new files (`../src/tools/mesa-build`: the d3dkmt backend and winsys, `fd_wgl_framebuffer.cpp`, ...) | MIT, by their SPDX tags ("Copyright © 2026", no holder named) | `MIT` |
| Khronos' headers: `include/EGL/*.h`, `include/GLES*/*platform.h`, the Vulkan video headers some of Mesa's util includes; `gl.xml`, which Mesa's generators read | Apache-2.0 | `Apache-2.0` |
| `src/c11/impl/threads_win32.c` (C11 threads over Win32) | BSL-1.0 | `BSL-1.0` |
| `src/util/xxhash.h` (xxHash, Yann Collet) | BSD-2-Clause | below |
| `src/util/softfloat.c`, `softfloat.h` and `src/compiler/glsl/float64.glsl` (Berkeley SoftFloat 3e) | BSD-3-Clause | below |
| `src/util/blake3` (BLAKE3 1.8.2) | its upstream's: public domain by CC0 1.0, or Apache-2.0 (with or without the LLVM exception); Mesa's copy has no notice of its own | `Apache-2.0` |
| The parsers bison 3.8.2 made in the build (GLSL's, glcpp's, ir3's assembler, ARB programs') | GPL-3.0-or-later with the Bison exception, which lets a larger work carry the skeleton under any terms | below |
| The scanners win_flex 2.6.4 made in the build | flex's skeleton, under flex's BSD-style licence | |
| The D3DKMT core, `../src/src/fdkmt` | no licence notice of its own | |
| The C runtime (VS 2017's MSVC 14.16 `libcmt`, `libvcruntime`, `libcpmt`; SDK 10.0.16299's `libucrt`), linked in | Microsoft's, for distributing programs built with Visual Studio | |

Mesa's `licenses/` folder has these texts (`MIT`, `Apache-2.0`, `BSL-1.0`) as copied here; it also has GPL-1.0-or-later,
GPL-2.0-only and SGI-B-2.0, which no file of these DLLs is under.

## xxHash (`src/util/xxhash.h`)

```
xxHash - Extremely Fast Hash algorithm
Header File
Copyright (C) 2012-2023 Yann Collet

BSD 2-Clause License (https://www.opensource.org/licenses/bsd-license.php)

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are
met:

   * Redistributions of source code must retain the above copyright
     notice, this list of conditions and the following disclaimer.
   * Redistributions in binary form must reproduce the above
     copyright notice, this list of conditions and the following disclaimer
     in the documentation and/or other materials provided with the
     distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
"AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```

## Berkeley SoftFloat 3e (`src/util/softfloat.c`, `softfloat.h`, `src/compiler/glsl/float64.glsl`)

```
License for Berkeley SoftFloat Release 3e

John R. Hauser
2018 January 20

The following applies to the whole of SoftFloat Release 3e as well as to
each source file individually.

Copyright 2011, 2012, 2013, 2014, 2015, 2016, 2017, 2018 The Regents of the
University of California.  All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

 1. Redistributions of source code must retain the above copyright notice,
    this list of conditions, and the following disclaimer.

 2. Redistributions in binary form must reproduce the above copyright
    notice, this list of conditions, and the following disclaimer in the
    documentation and/or other materials provided with the distribution.

 3. Neither the name of the University nor the names of its contributors
    may be used to endorse or promote products derived from this software
    without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS "AS IS", AND ANY
EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE, ARE
DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE FOR ANY
DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```

## The Bison exception (the parsers bison made)

```
As a special exception, you may create a larger work that contains
part or all of the Bison parser skeleton and distribute that work
under terms of your choice, so long as that work isn't itself a
parser generator using the skeleton or a modified version thereof
as a parser skeleton.  Alternatively, if you modify or redistribute
the parser skeleton itself, you may (at your option) remove this
special exception, which will cause the skeleton and the resulting
Bison output files to be licensed under the GNU General Public
License without this special exception.

This special exception was added by the Free Software Foundation in
version 2.2 of Bison.
```

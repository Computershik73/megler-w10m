# Building Mesa freedreno for Windows 10 Mobile ARM32

Requirements: VS 2017 (MSVC 14.16 ARM), SDK 10.0.16299, LLVM (clang-cl, lld-link, llvm-lib)
in `%ProgramFiles%\LLVM\bin`, `tools\win_flex_bison-2.5.25\`, Python 3 + `pip install --user meson ninja mako pyyaml packaging`.

Get Mesa and apply the patches:

    git clone --depth 1 -b mesa-26.2.7 https://gitlab.freedesktop.org/mesa/mesa.git third_party/mesa
    cd third_party/mesa && git checkout -b wp-arm32 && git am ../../tools/mesa-build/*.patch

Build (from the repo root):

    tools\mesa-build\build.cmd src/gallium/drivers/freedreno/libfreedreno.a src/freedreno/drm/libfreedreno_drm.a src/freedreno/ir3/libfreedreno_ir3.a

Output: `build\mesa-arm32\`. Interface to the D3DKMT core: `third_party/mesa/src/freedreno/drm/d3dkmt/fd_kmt.h`.

## Level 2: `fdtest.exe` (Mesa → fd_kmt → Qualcomm KMD)

Source: `third_party/mesa/src/freedreno/drm/d3dkmt/fdtest.c`. Steps:
1. `fd_kmt_open` → `fd_device_new_d3dkmt` → `fd_pipe_new`, print GPU/chip id, GMEM.
2. `fd_bo_new` 4 KiB + `fd_bo_map`, write/read a pattern from the CPU.
3. Submit a PM4 IB: `CP_NOP`, `CP_MEM_WRITE(bo, 0xC0FFEE01)`, `CP_WAIT_FOR_IDLE`.
4. `fd_fence_wait`, `bo[0]` must read `0xC0FFEE01`.

Exit 0 = PASS; `10+N` = failed at step N. With the stub: `FAIL buoc 1: fd_kmt_open (-40)` (ENOSYS), exit 11.

Build with the stub:

    tools\mesa-build\build.cmd src/freedreno/drm/fdtest.exe

Build with the real core: the core exports the `fd_kmt_*` functions from `fd_kmt.h` as an ARM32 static `.lib`, then:

    meson configure build\mesa-arm32 -Dfreedreno-d3dkmt-stub=false -Dfreedreno-d3dkmt-lib=D:/.../fd_kmt.lib
    tools\mesa-build\build.cmd src/freedreno/drm/fdtest.exe

Notes for core implementers:
- `fd_kmt_bo_alloc` must return an `iova` already mapped in GPU VA, with the BO resident (step 3 writes straight to the iova).
- `fd_kmt_submit` takes a list of IBs (`iova`, `size_dw`); the IBs already contain complete PM4, no patching/relocation (softpin).
- `fd_kmt_fence_wait(fence)` waits until the submit with that fence number is done; `fence` is monotonically increasing.
- On the device: run with `wpr -start GPU -filemode` / `wpr -stop x.etl` if steps 3–4 fail.

# fd_kmt — D3DKMT core for Mesa freedreno on Windows 10 Mobile

Implements the interface `third_party/mesa/src/freedreno/drm/d3dkmt/fd_kmt.h` using the public D3DKMT API
(WDDM 2.0 GpuMmu): virtual context, paging queue, `CreateAllocation2` + `MakeResident` +
`MapGpuVirtualAddress`, `Lock2`, `SubmitCommand` (GPU VA of the PM4 IB), monitored fence.

```
fd_kmt.c          generic WDDM part (GPU-independent): adapter, device, context, BO + cache,
                  submit, fence, statistics (MESA_LOG_DIR\fdkmt.log, only where MESA_LOG_DIR is set: Megler's change)
fd_kmt_priv.h     interface: struct fdkmt_gpu (GPU table) + struct fdkmt_kmd_ops (KMD ABI)
gpu_table.c       SoC -> GPU table (gpu_id for Mesa, GMEM, frequency, KMD ops, verified)
kmd_qcom.c        Qualcomm KMD ABI v1 (allocation private data 0x130 + CRC, cache flag 0x80)
probe/            fdkmt_probe.exe: tests the core without Mesa (CP_MEM_WRITE -> 0xC0FFEE01)
tools/mkalloc.py  generates an allocation private-data file (use with tools/kmtalloc -priv)
build.cmd         -> build\fdkmt\fd_kmt.lib + fdkmt_probe.exe
```

## Runtime GPU detection

`fd_kmt_open` picks the first hardware adapter, reads the UMD name
(`D3DKMTQueryAdapterInfo(KMTQAITYPE_UMDRIVERNAME)`, e.g. `qcdx11um8996.dll`), takes the trailing digits
(`8996`) and looks them up in `gpu_table.c`. SoC not in the table → `fd_kmt_open` fails and EGL reports
`EGL_NOT_INITIALIZED`.

Environment variables: `FDKMT_SOC=8996` (force SoC), `FDKMT_DEBUG=1` (log to stderr),
`FDKMT_PRIV_SIZE=N` (experiment with submit private data), `FDKMT_NODE=N`.

## Current GPU table

| SoC | GPU | Device | Status |
|---|---|---|---|
| 8996 | Adreno 530 (a5xx) | HP Pro x3 | ✅ verified (probe, egltest, UWP app 50 fps) |
| 8994 | Adreno 430 (a4xx) | Lumia 950 XL | ⏳ not verified |
| 8992 | Adreno 418 (a4xx) | Lumia 950 | ⏳ not verified; Mesa does **not** have `GPUId(418)` yet |

## Adding a new GPU / device

1. **Dump the device's drivers** into `vendor/<device>/<os>-<build>-<arch>/`: the names
   `qcdxkm<SoC>.sys`, `qcdx11um<SoC>.dll` tell you the SoC.
2. **Add a row to `gpu_table.c`** with `verified = false`: `gpu_id` as in Mesa
   (`third_party/mesa/src/freedreno/common/freedreno_devices.py`), GMEM, max frequency,
   `kmd = &fdkmt_kmd_qcom_v1`.
   - If Mesa does not know the GPU yet (e.g. 418): add `GPUId(...)` to the same-generation group in
     `freedreno_devices.py` (as a Mesa patch in `tools/mesa-build/`).
3. **Check the KMD ABI** on the device, in order (stop at the first failing step):
   1. `tools/kmtquery` — adapter, WDDM version, segments.
   2. `tools/kmtctx` — `CreateContextVirtual` (needs WDDM 2.0 GpuMmu).
   3. `kmtalloc -priv x.bin` with `python3 src/fdkmt/tools/mkalloc.py x.bin` — allocation private data.
   4. `FDKMT_DEBUG=1 fdkmt_probe` — PM4 submit + fence (`fdkmt_probe nop` is safer).
   5. `fdtest` (Mesa libfreedreno_drm), `tools/egltest` (offscreen GLES), `tools/glesapp` (on screen).
4. **KMD with a different format** (fails at step 3.3/3.4): create `kmd_<name>.c` with a new `struct fdkmt_kmd_ops`
   (allocation private data, submit private-data size, `bo_align`, `has_cpu_cached`), add it to
   `build.cmd`, point the `gpu_table.c` row at the new ops. `fd_kmt.c` needs no changes.
5. All passing → set `verified = true` and record the results in PROGRESS.md.

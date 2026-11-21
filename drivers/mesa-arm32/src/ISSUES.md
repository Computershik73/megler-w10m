# Known issues (2026-11-21)

## 1. GPU hang in GMEM mode — NOT FIXED (sysmem is the default)
- Symptom: dEQP `color_clear` in GMEM hangs randomly in ~25–35% of runs; it always starts in the *draw-based clear*
  group (scissored/masked), never in fast clears. The hung packet runs ~2.5 s and is then killed by the KMD (TDR).
- ETW: hangs correlate strongly (~65×) with our IB running **right after a dwm IB** on the GPU.
- **Ruled out** (no change in hang rate): IB size, BO cache, binning (`nobin`), LRZ, context priority,
  preemption save-record, `EnablePreemption=0` (registry), UCHE (registers protected by the KMD), BO addresses
  (every address in the IB belongs to a live BO), GMEM offset, dropping `CP_SET_RENDER_MODE`, flushing CCU before changing
  `RB_CCU_CNTL`, wait-for-idle + cache flush at batch start, Linux-style init registers.
- Tentative conclusion: caused by the Qualcomm KMD/firmware environment (system context state). Workaround: sysmem (default);
  enable GMEM with `FD_D3DKMT_GMEM=1` to experiment.
- Note: forcing 512 KiB GMEM (`FD_MESA_GMEM=524288`) gives deterministic wrong results — the `RB_CCU_CNTL` constant
  is tuned for 1 MiB, so it cannot be used to draw conclusions.

## 2. Performance
- Sysmem is bandwidth-bound (~2 ms per 1080x1920 blend layer, ~8.5 GB/s); GMEM ~0.4 ms/layer. Fixing (1) is the biggest win.
- Present: copying into the FLIP swapchain is held until vsync (~8–11 ms wait at 60 Hz) — that is vsync waiting, not real cost.

## 3. Not done / limitations
- Real `chip_id` not decoded yet (does not change emitted commands for a530).
- Minimum 4 KiB per BO allocation — needs a suballocator.
- Lumia 950 XL / Adreno 430 (a4xx) not supported yet.
- dEQP: GLES2 nearly clean; GLES3 on sysmem only has failures that are in the CTS upstream/hw-issues lists
  (e.g. polygon_offset `*_factor_1_slope`).

## 4. Lessons / pitfalls
- **Do not statically link `gdi32.lib` for `D3DKMT*`** on W10M: calls return success but do nothing → use `GetProcAddress(gdi32)`.
- `D3DKMTOpenResource`: `PrivateRuntimeDataSize` must equal the value returned by Query.
- A silent crash with exit code 255 is usually a CRT invalid-parameter; `libgallium_wgl.dll` prints a `module+offset`
  backtrace, map it with `llvm-symbolizer --obj=X.dll`.
- MTP `CopyHere` does not overwrite and caches listings → use FTP (`tools/phone/push.py`).
- Fixed in patches: `util_queue` UAF crash (0014), clipping/guardband (0018), stream-out (0019, 0021), depth/stencil tiling (0020).

Full details of every experiment: `docs/PROGRESS.md`.

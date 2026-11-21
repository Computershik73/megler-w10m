# Known issues (2026-11-21)

## 1. GPU hang in GMEM mode — FIXED in the D3DKMT core (Megler, 2026-11-21); sysmem stays the default (see 2)
- Symptom (before): dEQP `color_clear` in GMEM hung randomly in ~25–35% of runs, always in the *draw-based clear* group
  (scissored/masked), never in fast clears; the hung packet ran ~2.5 s and was killed by the KMD (TDR); in ETW, hangs
  correlated strongly (~65×) with our IB running **right after a dwm IB**. GTA: Vice City in GMEM on the HP Elite x3 hung
  12–33 s after it began, in 5 of 5 runs: `SubmitCommand ... failed: 0xc01e0200` (STATUS_GRAPHICS_GPU_EXCEPTION_ON_DEVICE)
  from then on, `D3DKMTGetDeviceState`: execution state 3, HUNG (not a page fault).
- **Cause.** The d3dkmt backend says it has `FD_VERSION_UNLIMITED_CMDS` (its version is `FD_VERSION_CACHED_COHERENT`), so
  freedreno makes its command streams growable: a batch's primary ring (the IB1 of a GMEM pass) that fills its first
  buffer goes on in a second, and so on (each GTA frame was three: 844, 8192 and 2878 dwords). On Linux the kernel runs
  all of a submit's buffers back to back in its ring. The D3DKMT core gave each one its own `D3DKMTSubmitCommand`, which
  the kernel makes a DMA packet of its own -- and between packets the GPU scheduler runs other contexts' packets, the
  compositor's at every frame. A GMEM pass cut there went on, in its next piece, after dwm's work had used GMEM, the
  CCU and the registers the first piece had set up (Qualcomm's KMD does not keep a context's registers across contexts:
  its own driver re-emits its whole state in every tile), and the GPU hung. Hence GMEM alone (a sysmem pass's IB1 is
  small, its state in memory), and the dwm packet right before the hung one. The experiments in the list below changed
  the hang rate no more than they changed where the IB1 was cut.
- **Proof.** GTA at 2560x1440 (2392 while the phone's bars show), the same DLLs: with the pieces submitted apart
  (`FDKMT_SPLIT_SUBMITS=1`) hung at 21.7 s; with one buffer, 0 hangs in 4 runs of 222, 490, 378 and 314 s (some 36000
  frames). The test app `megler.test.gmem` (scissored/masked clears read back, full-screen 1440x2112) never hung even
  before: its IB1 never outgrew one buffer.
- **Fix** (`fd_kmt.c`, `kmt_one_buffer`): a submit's pieces copied, in order, into one write-combined buffer (four in
  turn, each reused once its fence has passed; 256 KB, more if needed), submitted as one command. `FDKMT_SPLIT_SUBMITS=1`
  submits them apart as before. The first refused submit now says the device's state (hung, DMA or page fault and
  where) and the last 16 submits with their age (`device_lost_report`; `FDKMT_DUMP_HANG=1` also dumps them).
- dEQP not run again (no dEQP runner in Megler's setup).
- **Ruled out** by the contributor before (no change in hang rate): IB size, BO cache, binning (`nobin`), LRZ, context
  priority, preemption save-record, `EnablePreemption=0` (registry), UCHE (registers protected by the KMD), BO addresses,
  GMEM offset, dropping `CP_SET_RENDER_MODE`, flushing CCU before changing `RB_CCU_CNTL`, wait-for-idle + cache flush at
  batch start, Linux-style init registers. Megler (2026-11-21): local preemption off (`CP_PREEMPT_ENABLE_LOCAL 0`) at the
  IB1's start, around blits and at every draw -- still hung (33 s).
- Note: forcing 512 KiB GMEM (`FD_MESA_GMEM=524288`) gives deterministic wrong results — the `RB_CCU_CNTL` constant
  is tuned for 1 MiB, so it cannot be used to draw conclusions.

## 2. Performance
- Sysmem is bandwidth-bound (~2 ms per 1080x1920 blend layer, ~8.5 GB/s); GMEM ~0.4 ms/layer.
- GMEM with (1) fixed is still **slower** than sysmem for GTA: Vice City at 2560x1440 (2026-11-21, the same DLLs, 150-400 s
  after start, in the game at its save point): sysmem ~29 frames a second, the frame ring waiting 5-7 ms for the GPU;
  GMEM ~19, waiting 15-19 ms. Mesa's statistics of its GMEM batches (an experiment's `FD5_GMEM_STATS`): 30 bins of 512x256,
  hardware binning on, and every batch with `restore` color+depth+stencil and `resolve` color+depth -- GTA never clears,
  and Megler's window surfaces are pbuffers (shared Direct3D textures), whose contents Mesa keeps -- some 55 MB of memory
  traffic a batch that sysmem does not have, for batches of 5 to 54 draws.
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
- An allocation opened by its share handle (a D3D11 texture: a window surface's) cannot be `Lock2`ed: 0xc000000d. Megler's
  `megler-0027` marks such a BO `FD_BO_NOMAP`, so reads and writes of it go through a staging copy without asking.
- A silent crash with exit code 255 is usually a CRT invalid-parameter; `libgallium_wgl.dll` prints a `module+offset`
  backtrace, map it with `llvm-symbolizer --obj=X.dll`.
- MTP `CopyHere` does not overwrite and caches listings → use FTP (`tools/phone/push.py`).
- Fixed in patches: `util_queue` UAF crash (0014), clipping/guardband (0018), stream-out (0019, 0021), depth/stencil tiling (0020).

## 5. What Qualcomm's own Direct3D driver tells the KMD (Megler's flag `kmt-trace`, 2026-11-21)
Seen on the HP Elite x3 (W10M 15254, driver 11.18.1052.29) in ANGLE's device and Megler's shell, by tracing every
`NtGdiDdDDI*` call of the process (`core/src/host/mg_kmt_trace.c`, `tools/kmt-trace.py`). Facts, for whoever brings
the core closer to it; none of them was needed for (1):
- Contexts: two legacy `CreateContext` on nodes 1 and 2 (64 bytes of private data each, then a driver escape naming
  the context), and the 3D one by `CreateContextVirtual` on node 0 with flags 0x2, client hint 10 and 64 bytes of
  private data (dwords 1, 2, 3 and 7: 3, 2, 0x20000, 0x3b4). Mesa's core passes none, hint OpenGL.
- Every `SubmitCommand` carries 172 bytes of private data: a header (magic 0xEEBBAACC, its size), a section marked
  0x02ABDCFA holding one entry 0x0001CCCC that lists the command buffer (its size in dwords and its GPU address, the same
  as `Commands`), and an end marker 0x00ABDCFA. Mesa's core passes none (0x20 and 948 bytes were refused).
- Driver escapes (type 0) of 208-272 bytes before the 3D context is made, and an adapter query of 1840 bytes of the
  KMD's private adapter information (0x100000, the GMEM size it seems, among it).
- Command buffers: no `CP_SET_RENDER_MODE` at all, no hardware binning (`CP_SET_VISIBILITY_OVERRIDE 1`), tiles of
  768x320 for a 1440x2112 RGBA8 window. Each tile calls a clear IB2, a 900-dword preamble IB2 with the whole 3D state
  (`RB_CCU_CNTL` 0x7c100080 in GMEM, 0x10000000 for blits in bypass; Mesa writes 0x7c13c080), the draw IB2 and a store
  IB2. Every blit IB2 begins with a wait for idle, the blit mode registers and `CP_PREEMPT_ENABLE_LOCAL 0`, and every
  blit and draw IB2 ends with `PC_CCU_FLUSH_DEPTH_TS`, `PC_CCU_FLUSH_COLOR_TS` and a wait for idle. The IB1 ends with an
  LRZ flush, both CCU flushes and `CACHE_FLUSH_TS`.

Full details of every experiment: `docs/PROGRESS.md`.

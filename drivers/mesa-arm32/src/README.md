# Native OpenGL ES on Windows 10 Mobile (ARM32)

Mesa **freedreno** (Gallium, no ANGLE) running directly on the Adreno GPU of Windows 10 Mobile 1709,
talking to Qualcomm's stock KMD (`qcdxkm*.sys`) through the public **D3DKMT** API. Presentation goes through
EGL/WGL CoreWindow + a DXGI swapchain (W10M has no HWND/GDI).

| Device | SoC | GPU | Status |
|---|---|---|---|
| HP Elite x3 / Pro x3 | Snapdragon 820 (MSM8996) | Adreno 530 (a5xx) | Working: GLES2/GLES3, ~60 fps, dEQP GLES2 nearly clean |
| Lumia 950 XL | Snapdragon 810 (MSM8994) | Adreno 430 (a4xx) | Not started |

## Layout
- `src/fdkmt/` — D3DKMT core (generic WDDM `fd_kmt.c`, SoC→GPU table `gpu_table.c`, Qualcomm KMD ABI `kmd_qcom.c`). Adding a new GPU: `src/fdkmt/README.md`.
- `tools/mesa-build/` — **24 patches** on top of Mesa 26.2.7 (d3dkmt winsys/backend, ARM32 clang-cl build, a5xx fixes…) + build script.
- `tools/glesapp/` — UWP test app (draws GLES through CoreWindow).
- `tools/deqp-build/`, `tools/phone/` — build/run dEQP on the device, push files, telnet, summarize logs.
- `tools/kmtquery`, `kmtalloc`, `kmtctx`, `etl/`, `pm4/` — tools for probing D3DKMT, ETW, PM4.
- `tools/WpMirror/` — view/control the phone screen from the PC.
- `docs/PLAN.md`, `docs/PROGRESS.md` — plan and detailed step-by-step log.

See **[BUILD.md](BUILD.md)** to build/install and **[ISSUES.md](ISSUES.md)** for known issues.

## Not included (copyright reasons)
Qualcomm/HP/Microsoft drivers/firmware, WDK headers, reverse-engineering notes, the app signing certificate (`.pfx`),
and the Mesa source (download from upstream and apply the patches). Create your own test certificate to sign the appx.

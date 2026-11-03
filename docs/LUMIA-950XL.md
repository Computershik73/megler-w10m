# Lumia 950 XL: video and streaming apps (mayconus's work)

mayconus tested Megler on a Lumia 950 XL Dual SIM (RM-1116, Windows 10 Mobile 1709, 15254.603) with builds of their
own of the tree as it was at 0.1.0.578. Their builds' numbers (up to 597) are their own, apart from the releases
that carry the same numbers. What they changed came in as the commits merged with this file; this is what they found,
how they checked it, and what is still open.

## Video in MediaPlayer and VideoView

- **HEVC after a flush.** The H.265 transform gave a black first picture after a flush and lost the pictures after
  it, for Main and for Main 10. CodecHost now makes the transform again on a flush, with the configuration it was
  given. The phone's codec checks: 4 of 77 failed before, none after; after a flush pictures 12 to 23 all came back
  in order with the expected size and brightness.
- **Pictures on the Surface.** megler.Audio's decoders read pictures from the same media source as the sound
  (`decVideo`), and MediaPlayer, an overlay now, draws them on its Surface when the clock reaches their time: the
  mixer's position while there is sound, a monotonic clock without it. VideoView is backed by it. Stop, reset and
  release are serialized with the worker that decodes and writes, and a generation number silences what an old
  worker or a cancelled prepareAsync would still report.
- **Checked:** `tools/test-media-player.py` (24 checks on a JVM with host doubles: sound only, picture only, both,
  pause, seek while paused, looping, quick stop and prepare again, a cancelled preparation, HTTP headers through a
  loopback server, VideoView's deferred start). On the phone, the VideoView app in `core/tests/app/video` (built by
  `tools/build-video-test.py`, which needs ffmpeg): 8 checks, none failed, with an H.264/AAC MP4 and a picture-only
  one. Decoding with the conversion to a bitmap: H.264 720p 30 pictures a second, H.264 1080p 13, H.265 1080p 31
  (short synthetic clips, not a streaming app's rate).
- **Not done:** MediaExtractor is still a stub, so ExoPlayer and other players with their own extractors get
  nothing from this. A source is read whole into memory (up to 256 MiB): no progressive download, HLS or DASH. No
  DRM, VP9, AV1, subtitles or encoders. Pictures are converted from NV12 on the CPU (`mg_mcodec_render`); decoding
  into Direct3D 11 surfaces (IMFDXGIDeviceManager) would be the next step, once the phone's transform is known to
  support it.

## Streaming apps

Each app was started and watched for 35 to 61 seconds. Nobody signed in, and the catalogues, feeds and players were
not tried: a first picture does not mean an app works.

| App | What stopped it | What changed | Where it got |
| --- | --- | --- | --- |
| HBO Max 7.14.0.530 | WorkManager could not open SQLite: their build had kept objects made without SQLite | `tools/invalidate-core-features.py` | first picture, no fatal end |
| Crunchyroll 3.127.0 | the same; and the keyboard covered its sign-in page | the same; a focused WebView resizes for the keyboard | onboarding, its intro video decoded (42 H.264 pictures, 116 AAC blocks, no decoder failures), the sign-in page |
| TikTok Lite 47.6.51 | a native fault in libkeva: its dependencies, unpacked into `files/esolib/armeabi-v7a`, were not found | a library loaded by its path finds its neighbours | first picture; the whole button of the terms dialog (AlertDialog's padding) |
| Netflix 9.89.0 | `System.load(null)` from a class in an in-memory DEX | in-memory DEX files keep their defining loader; `__system_property_foreach` | libb668.so loads; then two faults at PC 0x28 with libb668.so on the stack, and no picture: **still not working** |
| Grand Truck Simulator 1.13 | Unity registered `nativeRecreateGfxState` with the Surface alone | the driver calls the older signature too | not seen running: it stopped while loading libunity.so, tried in portrait only |

Network was checked for TikTok Lite with a trace (names resolved, TLS connections sent and received bytes).

## Scrolling and drawing

- Touches keep the time they were queued, so moves that waited for one frame are no longer squeezed together, and
  VelocityTracker counts in the gesture's own time (a float of the uptime loses milliseconds after some hours).
- Choreographer runs what a frame's phase posts for a later phase in the same frame.
- SurfaceTexture keeps one Canvas per buffer instead of making one at every post (`tools/test-surface-texture.py`:
  two canvases for 240 frames, resizing and a release while the producer draws).
- A bitmap's current canvas is drawn into another on the GPU (`draw_canvas`), without reading it back; a
  RenderNode's temporary is let go without a read-back (`canvas_discard`); a large image that keeps changing goes
  into an upload texture of the canvas's own. In TikTok Lite with a video playing, eight scrolling gestures: 17.4
  presents a second with `draw_canvas`, 7.4 without it; the median of the gestures' 95th percentile intervals 164 ms
  against 269 ms, the longest interval still 918 ms. Better, not smooth. Their last build, with the upload textures,
  was not measured on the phone.
- At a 960-pixel render size instead of 1280 the apps' start showed no steady gain.

## Diagnostic flags and tools

Flags (files in the app's `LocalState\flags`), all off in a normal run:

- `trace-fps`: presents a second in the log; with `render-960` as well, a 960-pixel render size without changing the
  settings.
- `ui-scroll-up-<id>`, `ui-scroll-down-<id>`: a drag through the normal input queue, then the presents it got and the
  median, 95th percentile and longest interval between them.
- `ui-tap-<x>-<y>-<id>`: a tap at surface pixels; `ui-frame-<id>`: the app's picture saved as `shot-frame-<id>.bmp`.
- `no-canvas-copy`: the old way of drawing canvases into canvases, for comparison.
- `dump-memory-dex`: each in-memory DEX file saved as `memory-<n>.dex` in the app's data folder.

Tools: `tools/phone-perf-report.py` (the timings in a megler.log, as JSON), `tools/phone-scroll-check.py` (gestures
in an installed app, with its log and picture), `tools/probe-installed-apps.py` (starts installed apps, keeps their
fresh logs and what failed), and `MEGLER_PORTAL` for `tools/phone.py`, taken without a scan of the USB ports when it
answers with the right name.

## Not merged

`tools/package-video-checks.py` and `BUILD_HANDOFF_WINDOWS.ps1`: mayconus packed their builds with the runtime of the
official 0.1.0.578 package, which a full build here makes itself.

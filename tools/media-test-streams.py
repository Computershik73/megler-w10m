#!/usr/bin/env python3
"""
Writes the compressed streams the decoders' tests feed to MediaCodec, as an app's extractor would hand them over:
one access unit (a picture) or one packet (of sound) at a time, each with its time, and the codec-specific data
apart from them. Made with ffmpeg (libx264, libx265, its own AAC encoder, libmp3lame) and kept in the repository;
nothing here runs during a build.

    python tools/media-test-streams.py [output folder]     (core/tests/app/data/assets unless told)
    python tools/media-test-streams.py --speed <folder>

  avc.hkau    H.264 Main, 168 x 94 (coded 176 x 96, so the decoder has a picture to crop), 30 pictures a second,
              24 pictures, two B-pictures between references (so decode order is not display order), an IDR every
              12 pictures (pictures 0 and 12). Picture N is one flat grey, Y = 16 + 8 N, U = V = 128: what a
              decoder gives back says which picture it is. Its SPS and PPS are taken out of the first access unit
              and are the codec-specific data (csd-0, csd-1, each with its start code), as an MP4's avcC gives
              them to an app; access unit delimiters and SEI are left out.
  avc-small.hkau  12 pictures more of the same kind at 96 x 64, picture N again Y = 16 + 8 N: what an app's stream
              that changes its size (adaptive streaming does) goes on with.
  hevc.hkau   the same pictures as avc.hkau as H.265 Main (libx265), VPS, SPS and PPS together as csd-0.
  hevc10.hkau the same as H.265 Main 10, ten bits a sample (Y = 64 + 32 N of 1023), which decoders hand out as P010.
  aac.hkau    AAC LC, 44100 Hz, stereo, half a second of a 440 Hz tone (made at half of full scale, and 3 dB less in
              each channel once ffmpeg makes it stereo: 8192 RMS): raw access units (the ADTS headers taken off) and the
              AudioSpecificConfig as csd-0.
  mp3.hkau    the same tone as MPEG-1 Layer III at 128 kbit/s, one frame a packet, no ID3 or Xing header.

With --speed, what a decoder's speed is measured on instead, two seconds of ffmpeg's moving test picture (testsrc2),
as a phone plays films; these are made where they are needed (tools/build-appx.sh --auto codec, tools/build-tests.sh
win) and not kept in the repository:

  speed-avc-720p.hkau     H.264 High, 1280 x 720, 60 pictures, three B-pictures between references, an IDR a second
  speed-avc-1080p.hkau    the same at 1920 x 1080
  speed-hevc-1080p.hkau   H.265 Main at 1920 x 1080

The file: "HKMEDIA1", the MIME type in 32 bytes (zero-padded), then little-endian u32 width, height, sample rate,
channels and the number of records; then each record: u32 flags (MediaCodec's: 1 key frame, 2 codec-specific
data), u32 size, i64 time in microseconds, and the bytes. Records with flag 2 come first; the rest are in decode
order.
"""
import json
import os
import struct
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "core", "tests", "app", "data", "assets")
FLAG_KEY, FLAG_CONFIG = 1, 2


def run(*args):
    subprocess.run(args, check=True)


def write(name, mime, width, height, rate, channels, records):
    path = os.path.join(OUT, name)
    with open(path, "wb") as f:
        f.write(b"HKMEDIA1" + mime.encode().ljust(32, b"\0"))
        f.write(struct.pack("<5I", width, height, rate, channels, len(records)))
        for flags, time_us, data in records:
            f.write(struct.pack("<IIq", flags, len(data), time_us))
            f.write(data)
    print("%s: %d records, %d bytes" % (path, len(records), os.path.getsize(path)))


def packets(ts):
    """The video packets of an MPEG-TS file, in decode order: (pts in 90 kHz units, key, bytes)."""
    out = subprocess.run(["ffprobe", "-v", "error", "-select_streams", "v", "-show_packets", "-show_data", "-of", "json", ts],
                         check=True, capture_output=True, text=True).stdout
    result = []
    for p in json.loads(out)["packets"]:
        data = b""
        for line in p["data"].splitlines():
            if len(line) > 10:
                data += bytes.fromhex(line[10:49].replace(" ", ""))
        result.append((int(p["pts"]), "K" in p["flags"], data))
    return result


def nal_units(data):
    """The NAL units of an Annex-B access unit, without their start codes."""
    starts, i = [], 0
    while i + 3 <= len(data):
        if data[i] == 0 and data[i + 1] == 0 and data[i + 2] == 1:
            starts.append(i + 3)
            i += 3
        else:
            i += 1
    units = []
    for k, s in enumerate(starts):
        end = starts[k + 1] - 3 if k + 1 < len(starts) else len(data)
        while end > s and data[end - 1] == 0:
            end -= 1
        units.append(data[s:end])
    return units


def video(name, mime, encoder, params, nal_type, config_types, drop_types, width=168, height=94, count=24, source=None):
    with tempfile.TemporaryDirectory() as tmp:
        ts = os.path.join(tmp, "v.ts")
        source = source or "nullsrc=s=%dx%d:r=30,format=yuv420p,geq=lum='16+8*N':cb=128:cr=128" % (width, height)
        run("ffmpeg", "-v", "error", "-y", "-f", "lavfi", "-i", source, "-frames:v", str(count), "-c:v", encoder, *params, "-f", "mpegts", ts)
        pk = packets(ts)
    first = min(p[0] for p in pk)
    config, records = [], []
    for pts, key, data in pk:
        kept = b""
        for unit in nal_units(data):
            t = nal_type(unit)
            if t in config_types:
                if not any(c == unit for c in config):
                    config.append(unit)
            elif t not in drop_types:
                kept += b"\0\0\0\1" + unit
        records.append((FLAG_KEY if key else 0, (pts - first) * 100 // 9, kept))
    if mime == "video/avc":
        csd = [(FLAG_CONFIG, 0, b"\0\0\0\1" + c) for c in config]     # csd-0 the SPS, csd-1 the PPS
    else:
        csd = [(FLAG_CONFIG, 0, b"".join(b"\0\0\0\1" + c for c in config))]
    write(name, mime, width, height, 0, 0, csd + records)


def adts_frames(data):
    i = 0
    while i + 7 <= len(data):
        if data[i] != 0xFF or (data[i + 1] & 0xF6) != 0xF0:
            raise SystemExit("not ADTS at byte %d" % i)
        protection_absent = data[i + 1] & 1
        length = ((data[i + 3] & 3) << 11) | (data[i + 4] << 3) | (data[i + 5] >> 5)
        header = 7 if protection_absent else 9
        yield data[i:i + 7], data[i + header:i + length]
        i += length


def mp3_frames(data):
    bitrates = [0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320]
    rates = [44100, 48000, 32000]
    i = 0
    while i + 4 <= len(data):
        if data[i] != 0xFF or (data[i + 1] & 0xFE) != 0xFA:            # MPEG-1 Layer III
            raise SystemExit("not an MPEG-1 Layer III frame at byte %d" % i)
        bitrate = bitrates[data[i + 2] >> 4] * 1000
        rate = rates[(data[i + 2] >> 2) & 3]
        padding = (data[i + 2] >> 1) & 1
        length = 144 * bitrate // rate + padding
        yield data[i:i + length]
        i += length


def speed(folder):
    global OUT
    OUT = folder
    os.makedirs(OUT, exist_ok=True)
    avc = lambda u: u[0] & 0x1F
    x264 = ["-preset", "fast", "-profile:v", "high", "-crf", "23", "-x264-params", "bframes=3:keyint=30:min-keyint=30:scenecut=0"]
    for w, h in ((1280, 720), (1920, 1080)):
        video("speed-avc-%dp.hkau" % h, "video/avc", "libx264", x264, avc, (7, 8), (6, 9), w, h, 60, "testsrc2=s=%dx%d:r=30" % (w, h))
    video("speed-hevc-1080p.hkau", "video/hevc", "libx265",
          ["-preset", "fast", "-crf", "28", "-x265-params", "log-level=error:bframes=3:keyint=30:min-keyint=30:scenecut=0"],
          lambda u: (u[0] >> 1) & 0x3F, (32, 33, 34), (35, 39, 40), 1920, 1080, 60, "testsrc2=s=1920x1080:r=30")


def main():
    if len(sys.argv) > 2 and sys.argv[1] == "--speed":
        speed(sys.argv[2])
        return
    os.makedirs(OUT, exist_ok=True)
    x264 = ["-preset", "medium", "-profile:v", "main", "-x264-params", "bframes=2:b-adapt=0:b-pyramid=none:keyint=12:min-keyint=12:scenecut=0:open-gop=0"]
    video("avc.hkau", "video/avc", "libx264", x264, lambda u: u[0] & 0x1F, (7, 8), (6, 9))
    video("avc-small.hkau", "video/avc", "libx264", x264, lambda u: u[0] & 0x1F, (7, 8), (6, 9), 96, 64, 12)
    x265 = "log-level=error:bframes=2:b-adapt=0:b-pyramid=0:keyint=12:min-keyint=12:scenecut=0:open-gop=0"
    video("hevc.hkau", "video/hevc", "libx265", ["-preset", "medium", "-x265-params", x265], lambda u: (u[0] >> 1) & 0x3F, (32, 33, 34), (35, 39, 40))
    video("hevc10.hkau", "video/hevc", "libx265", ["-preset", "medium", "-profile:v", "main10", "-x265-params", x265], lambda u: (u[0] >> 1) & 0x3F,
          (32, 33, 34), (35, 39, 40), source="nullsrc=s=168x94:r=30,format=yuv420p,geq=lum='16+8*N':cb=128:cr=128,format=yuv420p10le")
    tone = "sine=frequency=440:sample_rate=44100:duration=0.5,volume=4"
    with tempfile.TemporaryDirectory() as tmp:
        aac = os.path.join(tmp, "a.aac")
        run("ffmpeg", "-v", "error", "-y", "-f", "lavfi", "-i", tone, "-ac", "2", "-c:a", "aac", "-b:a", "96k", "-f", "adts", aac)
        frames = list(adts_frames(open(aac, "rb").read()))
        h = frames[0][0]
        profile, sfi, chan = h[2] >> 6, (h[2] >> 2) & 0xF, ((h[2] & 1) << 2) | (h[3] >> 6)
        asc = struct.pack(">H", ((profile + 1) << 11) | (sfi << 7) | (chan << 3))
        records = [(FLAG_CONFIG, 0, asc)] + [(FLAG_KEY, k * 1024 * 1000000 // 44100, f) for k, (_, f) in enumerate(frames)]
        write("aac.hkau", "audio/mp4a-latm", 0, 0, 44100, 2, records)
        mp3 = os.path.join(tmp, "a.mp3")
        run("ffmpeg", "-v", "error", "-y", "-f", "lavfi", "-i", tone, "-ac", "2", "-c:a", "libmp3lame", "-b:a", "128k",
            "-write_xing", "0", "-id3v2_version", "0", "-f", "mp3", mp3)
        records = [(FLAG_KEY, k * 1152 * 1000000 // 44100, f) for k, f in enumerate(mp3_frames(open(mp3, "rb").read()))]
        write("mp3.hkau", "audio/mpeg", 0, 0, 44100, 2, records)


main()

"""Summarize Megler performance logs without treating idle windows as jank.

Use separate captures for startup, interactive scrolling and gameplay.
Run with one or more megler.log paths. The JSON contains timings, not verdicts.
"""
import argparse
import json
from pathlib import Path
import re

RENDER = re.compile(
    r"gfx: the render thread, ([\d.]+) s: (\d+) frames .*?, "
    r"([\d.]+) ms drawing the lists and ([\d.]+) ms showing each")
GL = re.compile(
    r"java: OpenGL frames (?:\S+ )?([\d.]+) s: (\d+) swaps .*?, "
    r"(\d+) shown \(([\d.]+) a second\); a swap's own work ([\d.]+) ms")
JAVA = re.compile(
    r"java: frames .*?(\d+) drawn.*?, ([\d.]+) ms of work each.*?"
    r"measure and layout ([\d.]+), draw traversal ([\d.]+)")


def samples(pattern, keys, log):
    return [dict(zip(keys.split(), map(float, match.groups()))) for match in pattern.finditer(log)]


def summarize(path):
    log = path.read_text(encoding="utf-8", errors="replace")
    return {
        "path": str(path),
        "native_faults": len(re.findall(r" E fault:", log)),
        "first_picture": "window: the app's first picture" in log,
        "java": samples(JAVA, "frames work_ms layout_ms traversal_ms", log),
        "render": samples(RENDER, "seconds frames draw_ms present_ms", log),
        "gl": samples(GL, "seconds swaps shown shown_per_second swap_work_ms", log),
        "present_samples": samples(
            re.compile(r"fps sample ([\d.]+) \((\d+) presents in ([\d.]+) s\)"),
            "presents_per_second frames seconds", log),
        "scroll_samples": samples(
            re.compile(r"scroll sample ended, (\d+) presents in (\d+) ms"),
            "frames elapsed_ms", log),
        "scroll_intervals": samples(
            re.compile(r"scroll intervals: (\d+), p50 ([\d.]+) ms, p95 ([\d.]+) ms, max ([\d.]+) ms"),
            "intervals p50_ms p95_ms max_ms", log),
    }


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("logs", nargs="+", type=Path)
    args = parser.parse_args()
    print(json.dumps([summarize(path) for path in args.logs], indent=2))

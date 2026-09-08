#!/usr/bin/env python3
"""Point-to-point test suite for a Sega 32X ROM via PicoDrive (libretro).

Each script in tests/scripts/*.txt boots the real ROM in the emulator, drives
a scripted controller sequence through the C harness, and dumps PPM frames.
This runner then asserts the frames are LIT, COLOURFUL, and CHANGING - the
black-screen guard - plus per-script structural checks.

    python3 tests/run_tests.py            run every script
    python3 tests/run_tests.py 03_match   run one script (by stem)

Layout expected (adjust the paths near the top to your project):
    tests/harness                 built C harness (see tests/Makefile)
    tests/emu/picodrive_libretro.so
    tests/scripts/*.txt           input scripts
    rom/<game>.32x                the ROM under test
Captured frames + stats land in tests/out/<script>/.
"""
from __future__ import annotations

import os
import subprocess
import sys
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

# ---- project-specific paths ----------------------------------------------
ROM = os.environ.get("TUXRACER_ROM", os.path.join(ROOT, "rom", "tuxracer.32x"))
CORE = os.path.join(HERE, "emu", "picodrive_libretro.so")
HARNESS = os.path.join(HERE, "harness")
SCRIPTS = os.path.join(HERE, "scripts")
OUT = os.path.join(HERE, "out")

# ---- black-screen thresholds ---------------------------------------------
# Tux Racer 32X is flat-shaded 3D on a sky gradient: every real screen has a
# lit sky, so the lit ratio is high; the colour count is lower than for
# textured 2D art, hence the modest distinct-colour floor.
MIN_NONBLACK_RATIO = 0.50   # sky + snow: at least half the pixels are lit
MIN_DISTINCT_COLORS = 6     # flat-shaded scene still has >= 6 colours
BLACK_LEVEL = 8             # r,g,b all <= this counts as "black"
# splash screen is black with white text: exempt from the lit-ratio floor
SPLASH_SHOTS = {"splash"}
MIN_SPLASH_LIT = 0.005

# race screens must show the world: snow (bright), sky (blue) and a Tux
# silhouette (dark) - checks the 3D scene actually rendered, not just HUD text
def looks_like_race(st):
    px = st["_px"]
    n = st["w"] * st["h"]
    snow = sky = dark = 0
    for i in range(0, len(px), 3):
        r, g, b = px[i], px[i + 1], px[i + 2]
        if r > 200 and g > 200 and b > 200:
            snow += 1
        elif b > r + 30 and b > 100:
            sky += 1
        elif r < 60 and g < 60 and b < 80:
            dark += 1
    return snow / n > 0.10 and sky / n > 0.05 and dark / n > 0.002


class Failure(Exception):
    pass


def read_ppm(path):
    with open(path, "rb") as f:
        data = f.read()
    if not data.startswith(b"P6"):
        raise Failure(f"{path}: not a P6 PPM")
    fields, i = [], 2
    while len(fields) < 3:
        while i < len(data) and data[i:i + 1].isspace():
            i += 1
        if data[i:i + 1] == b"#":
            while data[i:i + 1] not in (b"\n", b""):
                i += 1
            continue
        s = i
        while i < len(data) and not data[i:i + 1].isspace():
            i += 1
        fields.append(int(data[s:i]))
    i += 1
    w, h, _ = fields
    return w, h, data[i:i + w * h * 3]


def stats(path):
    w, h, px = read_ppm(path)
    nonblack, colors = 0, set()
    for i in range(0, len(px), 3):
        r, g, b = px[i], px[i + 1], px[i + 2]
        if r > BLACK_LEVEL or g > BLACK_LEVEL or b > BLACK_LEVEL:
            nonblack += 1
        colors.add((r >> 3, g >> 3, b >> 3))
    total = w * h
    return {"w": w, "h": h, "nonblack_ratio": nonblack / total,
            "colors": len(colors), "crc": f"{zlib.crc32(px):08x}",
            "_px": px}


def crop_colors(st, x0, y0, x1, y1):
    w, px = st["w"], st["_px"]
    seen = set()
    for y in range(y0, y1):
        for x in range(x0, x1):
            o = (y * w + x) * 3
            seen.add((px[o] >> 3, px[o + 1] >> 3, px[o + 2] >> 3))
    return len(seen)


def pixel_delta(a, b):
    if (a["w"], a["h"]) != (b["w"], b["h"]):
        return 1.0
    pa, pb = a["_px"], b["_px"]
    changed = sum(pa[i:i + 3] != pb[i:i + 3] for i in range(0, len(pa), 3))
    return changed / (a["w"] * a["h"])


def assert_not_black(name, st, artwork=True):
    shot = name.split("/")[-1]
    if shot in SPLASH_SHOTS:
        if st["nonblack_ratio"] < MIN_SPLASH_LIT:
            raise Failure(f"{name} is black/blank: {st['nonblack_ratio']:.4f} lit")
        return
    if st["nonblack_ratio"] < MIN_NONBLACK_RATIO:
        raise Failure(f"{name} is black/blank: {st['nonblack_ratio']:.4f} lit")
    if artwork and st["colors"] < MIN_DISTINCT_COLORS:
        raise Failure(f"{name} has too few colors: {st['colors']}")


def run_script(path):
    name = os.path.splitext(os.path.basename(path))[0]
    outdir = os.path.join(OUT, name)
    os.makedirs(outdir, exist_ok=True)
    for old in os.listdir(outdir):
        if old.endswith(".ppm"):
            os.remove(os.path.join(outdir, old))
    proc = subprocess.run([HARNESS, CORE, ROM, path, outdir],
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    log = "\n".join(l for l in (proc.stdout + proc.stderr).splitlines()
                    if l and "FIR" not in l and "DRC" not in l)
    with open(os.path.join(outdir, "harness.log"), "w") as f:
        f.write(log + "\n")
    if proc.returncode != 0:
        raise Failure(f"{name}: harness exit {proc.returncode}\n{log}")

    shots = {s[:-4]: stats(os.path.join(outdir, s))
             for s in sorted(os.listdir(outdir)) if s.endswith(".ppm")}
    if not shots:
        # telemetry-only script (all its assertions live in the harness)
        print(f"PASS {name}: telemetry assertions only")
        return {}

    # Every captured frame must be non-black.
    for shot, st in shots.items():
        assert_not_black(f"{name}/{shot}", st)

    # If the script took >=2 shots, at least two must differ (not frozen).
    crcs = {st["crc"] for st in shots.values()}
    if len(shots) >= 2 and len(crcs) < 2:
        raise Failure(f"{name}: output never changed across {len(shots)} shots")

    # Every in-race / intro shot must show the rendered world.
    for shot, st in shots.items():
        if shot.startswith(("race", "intro", "frozen", "paused", "race_over")):
            if not looks_like_race(st):
                raise Failure(f"{name}/{shot}: no 3D scene visible (snow/sky/Tux test failed)")

    # Consecutive race shots must differ substantially (Tux is moving).
    race = [shots[k] for k in sorted(shots) if k.startswith("race_")]
    for a, b in zip(race, race[1:]):
        if pixel_delta(a, b) < 0.05:
            raise Failure(f"{name}: consecutive race frames nearly identical (scene frozen?)")

    print(f"PASS {name}: " + ", ".join(
        f"{k}[{v['colors']}c,{v['nonblack_ratio']:.2f}lit]"
        for k, v in shots.items()))
    return shots


def main(argv):
    if not os.path.exists(HARNESS):
        raise SystemExit(f"harness not built: {HARNESS} (run: make -C tests)")
    if not os.path.exists(CORE):
        raise SystemExit(f"emulator core missing: {CORE} (see references/testing.md)")
    if not os.path.exists(ROM):
        raise SystemExit(f"ROM not found: {ROM} (build it first)")

    want = argv[1] if len(argv) > 1 else None
    scripts = sorted(s for s in os.listdir(SCRIPTS) if s.endswith(".txt"))
    if want:
        scripts = [s for s in scripts if s.startswith(want) or s[:-4] == want]
        if not scripts:
            raise SystemExit(f"no script matches {want!r}")

    failures = 0
    for s in scripts:
        try:
            run_script(os.path.join(SCRIPTS, s))
        except (Failure, subprocess.CalledProcessError) as e:
            failures += 1
            print(f"FAIL {s}: {e}", file=sys.stderr)
    if failures:
        raise SystemExit(f"{failures} test(s) failed")
    print(f"all {len(scripts)} test(s) passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))

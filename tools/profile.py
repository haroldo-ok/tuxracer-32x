#!/usr/bin/env python3
"""
profile.py - frame-time profile of the 32X build under PicoDrive.

Builds the `prof` variant (rom/tuxracer-prof.32x: DEBUG_OVERLAY + the
per-stage timers in game.c), drives it into a race with the test harness,
takes screenshots at several points and decodes the 16-word telemetry strip
the prof build paints on rows 220..223 (20 px per word, 1 px per bit).

All counters are cumulative 16-bit vblank counts; the tool differences
consecutive screenshots, so the numbers are true averages over the frames
between two shots (including the game-logic ticks the renderer's speed
forces per frame).

Words: 0 frame(ticks), 1 vblank, 2 frames rendered, 3 num_tris (instant),
       4 update, 5 render, 6 flip, 7..13 stage0..6, 14 logic ticks, 15 0xA55A
Stages: 0 sky, 1 terrain geometry, 2 terrain flush (sort+raster),
        3 objects geometry, 4 Tux, 5 final flush, 6 HUD.

Usage: python3 tools/profile.py [--no-build] [--script file]
"""
import argparse, os, subprocess, sys, glob

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CORE = os.path.join(ROOT, "tests/emu/picodrive_libretro.so")
HARNESS = os.path.join(ROOT, "tests/harness")
OUT = os.path.join(ROOT, "obj/profile")

DEFAULT_SCRIPT = """\
run 200
press start 8
run 40
press start 8
run 60
waitlo 12 5 8000
run 30
shot p0
run 90
shot p1
hold left
run 90
shot p2
release left
run 90
shot p3
hold right
run 90
shot p4
release right
run 90
shot p5
run 90
shot p6
run 90
shot p7
"""

NAMES = ["sky", "terrGeo", "terrFlush", "objGeo", "tux", "flush2", "hud"]


def read_ppm(path):
    with open(path, "rb") as f:
        data = f.read()
    parts = data.split(b"\n", 3)
    w, h = map(int, parts[1].split())
    return w, h, parts[3]


def decode_strip(path):
    w, h, pix = read_ppm(path)
    vals = []
    y = 221
    for i in range(16):
        v = 0
        for b in range(16):
            x = i * 20 + b
            o = (y * w + x) * 3
            lum = pix[o] + pix[o + 1] + pix[o + 2]
            v = (v << 1) | (1 if lum > 300 else 0)
        vals.append(v)
    return vals


def d16(a, b):
    return (b - a) & 0xFFFF


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--no-build", action="store_true")
    ap.add_argument("--script", default=None)
    ap.add_argument("--flags", default="", help="extra PROF_FLAGS (e.g. -DPROF_SKIP=2)")
    args = ap.parse_args()

    os.makedirs(OUT, exist_ok=True)
    if not args.no_build:
        r = subprocess.run(["make", "prof", "PROF_FLAGS=%s" % args.flags], cwd=ROOT,
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        if r.returncode != 0:
            print(r.stdout[-3000:])
            sys.exit("prof build failed")
    rom = os.path.join(ROOT, "rom/tuxracer-prof.32x")
    script = args.script
    if script is None:
        script = os.path.join(OUT, "script.txt")
        with open(script, "w") as f:
            f.write(DEFAULT_SCRIPT)
    for p in glob.glob(os.path.join(OUT, "*.ppm")):
        os.remove(p)
    r = subprocess.run([HARNESS, CORE, rom, script, OUT], cwd=ROOT,
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if r.returncode != 0:
        print(r.stdout[-3000:])
        sys.exit("harness failed")

    shots = []
    for p in sorted(glob.glob(os.path.join(OUT, "p*.ppm"))):
        v = decode_strip(p)
        if v[15] != 0xA55A:
            print(os.path.basename(p), "bad marker", hex(v[15]))
            continue
        shots.append((os.path.basename(p)[:-4], v))
    if len(shots) < 2:
        sys.exit("need at least two shots")

    print("%-6s %5s %5s %6s %6s %6s %6s | %s" % ("span", "frms", "tris", "vbl/f", "upd/f", "rnd/f", "flip/f",
                                                 " ".join("%8s" % n for n in NAMES)))
    tot = {"vbl": 0.0, "upd": 0.0, "rnd": 0.0, "flip": 0.0, "ticks": 0.0}
    stot = [0.0] * 7
    nspan = 0
    for (na, a), (nb, b) in zip(shots, shots[1:]):
        frames = d16(a[2], b[2])
        if frames == 0:
            continue
        vbl = d16(a[1], b[1]) / frames
        upd = d16(a[4], b[4]) / frames
        rnd = d16(a[5], b[5]) / frames
        flp = d16(a[6], b[6]) / frames
        ticks = d16(a[14], b[14]) / frames
        st = [d16(a[7 + i], b[7 + i]) / frames for i in range(7)]
        for i in range(7):
            stot[i] += st[i]
        tot["vbl"] += vbl; tot["upd"] += upd; tot["rnd"] += rnd; tot["flip"] += flp; tot["ticks"] += ticks
        nspan += 1
        print("%-6s %5d %5d %6.2f %6.2f %6.2f %6.2f | %s" % (na + "-" + nb[1:], frames, b[3], vbl, upd, rnd, flp,
                                                            " ".join("%8.2f" % s for s in st)))
    if nspan:
        print("-" * 100)
        print("avg: %.2f vblanks/frame -> %.1f fps   (update %.2f for %.2f ticks/frame = %.2f/tick, render %.2f, flip %.2f)" % (
            tot["vbl"] / nspan, 60.0 * nspan / max(tot["vbl"], 0.01), tot["upd"] / nspan, tot["ticks"] / nspan,
            (tot["upd"] / max(tot["ticks"], 0.01)), tot["rnd"] / nspan, tot["flip"] / nspan))
        print("render stages (vblanks/frame): " + "  ".join("%s %.2f" % (n, s / nspan) for n, s in zip(NAMES, stot)))


if __name__ == "__main__":
    main()

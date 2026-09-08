#!/usr/bin/env python3
"""bench.py - run the micro-benchmark ROM (make bench) under PicoDrive and
decode the 16 results from the telemetry strip (rows 220..223).
Each value is the number of 68k vblanks (1/60 s) the benchmark loop took."""
import os, subprocess, sys, glob
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from profile import decode_strip, ROOT, CORE, HARNESS

OUT = os.path.join(ROOT, "obj/bench")
LABELS = [
    ("10k fill_tri 20x20", 10000), ("10k fill_tri 4x4", 10000), ("1k fill_tri 60x60", 1000),
    ("10k fill_tri 8x8", 10000), ("60k terrain vertices", 60000), ("100 sorts of 500", 100),
    ("100k cam_transform", 100000), ("100k render_project", 100000), ("10k render_tri_cam", 10000),
    ("10k course_find_y", 10000), ("20 terrain geometry", 20), ("terrain tris", 0),
    ("20 terrain flush", 20), ("20 objects+tux geometry", 20), ("20 object flush", 20), ("asm/C raster mismatches", 0),
]

def main():
    os.makedirs(OUT, exist_ok=True)
    if "--no-build" not in sys.argv:
        r = subprocess.run(["make", "bench"], cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        if r.returncode != 0:
            print(r.stdout[-3000:]); sys.exit("bench build failed")
    script = os.path.join(OUT, "script.txt")
    with open(script, "w") as f:
        f.write("run 1500\nshot bench\n")
    r = subprocess.run([HARNESS, CORE, os.path.join(ROOT, "rom/tuxracer-bench.32x"), script, OUT], cwd=ROOT,
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if r.returncode != 0:
        print(r.stdout[-2000:]); sys.exit("harness failed")
    v = decode_strip(os.path.join(OUT, "bench.ppm"))
    for (label, n), val in zip(LABELS, v):
        if n:
            us = val / 60.0 * 1e6 / n
            print("%-26s %5d vblanks  = %9.2f us each" % (label, val, us))
        else:
            print("%-26s %5d" % (label, val))

if __name__ == "__main__":
    main()

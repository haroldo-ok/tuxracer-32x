#!/usr/bin/env python3
"""Assemble every captured emulator frame (tests/out/*/*.ppm) into one PNG
contact sheet, tests/out/contact_sheet.png, for eyeballing after `make test`."""
import os, sys
HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "out")
try:
    from PIL import Image, ImageDraw
except ImportError:
    print("contact_sheet: Pillow not installed, skipping")
    sys.exit(0)
shots = []
for script in sorted(os.listdir(OUT)):
    d = os.path.join(OUT, script)
    if not os.path.isdir(d):
        continue
    for f in sorted(os.listdir(d)):
        if f.endswith(".ppm"):
            shots.append((script, f[:-4], os.path.join(d, f)))
if not shots:
    sys.exit(0)
cols = 4
w, h = 320, 224
rows = (len(shots) + cols - 1) // cols
sheet = Image.new("RGB", (cols * w, rows * (h + 14)), (30, 30, 30))
draw = ImageDraw.Draw(sheet)
for i, (script, name, path) in enumerate(shots):
    im = Image.open(path).convert("RGB").resize((w, h))
    x, y = (i % cols) * w, (i // cols) * (h + 14)
    sheet.paste(im, (x, y + 14))
    draw.text((x + 4, y + 1), f"{script}/{name}", fill=(255, 255, 255))
out = os.path.join(OUT, "contact_sheet.png")
sheet.save(out)
print(f"contact sheet: {out} ({len(shots)} frames)")

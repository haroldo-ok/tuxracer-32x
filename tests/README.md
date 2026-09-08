# Tux Racer 32X — end-to-end tests (PicoDrive)

`make check` is the release gate.  It rebuilds the release ROM from scratch,
boots it in a real 32X emulator (PicoDrive, libretro core) and drives it with
the controller through the whole game flow, asserting on both **pixels** and
the game's own **telemetry** (32X COMM registers).  A ROM only gets a
`rom/tuxracer.32x.verified` stamp if every script passed.

Why this exists: a benchmark build (`-DBENCH`, black screen with timing
numbers) was once left in `rom/tuxracer.32x`.  Nothing caught it because the
only checks at the time were static (header/size).  Now:

* instrumented builds go to their own object dir and ROM name
  (`make bench` → `rom/tuxracer-bench.32x`, `make prof` → `rom/tuxracer-prof.32x`)
  and can never overwrite the release image;
* the emulator suite fails on a black/blank frame, a frozen frame, a missing
  3D scene, or a wrong game mode (see below);
* `make verified` tells you whether the ROM in `rom/` is the image that passed.

## Layout

| path | purpose |
|---|---|
| `tests/harness.c` | minimal libretro frontend: loads the core + ROM, runs scripted pad input, dumps PPM frames, reads 32X COMM registers straight from the core's `Pico32x` symbol |
| `tests/emu/picodrive_libretro.so` | PicoDrive core (built from source with 32X support) |
| `tests/scripts/*.txt` | one scenario per file (see table) |
| `tests/run_tests.py` | runs every script, asserts frames are lit / colourful / changing, plus scene checks |
| `tests/verify_rom.py` | static checks: header, checksum, `.bss` below the SH-2 stacks, game strings survived linking |
| `tests/contact_sheet.py` | tiles every captured frame into `tests/out/contact_sheet.png` |
| `tests/out/<script>/` | captured `*.ppm` frames + `harness.log` per run |

## Telemetry the game publishes (src/platform/32x/main.c)

| COMM | meaning |
|---|---|
| 0 | `0x5458` 'TX' = SH-2 `main()` reached; `0x474F` 'GO' = video + palette up |
| 2 | game tick heartbeat (increments 60×/s) |
| 4 | boot handshake (`S_OK` from the slave) — never reused |
| 6 | sound state word for the slave SH-2 |
| 8 | pad state published by the 68000 (U1 D2 L4 R8 B10 C20 A40 START80) |
| 10 | 68000 vblank counter |
| 12 | `course_index << 8 \| game_mode` (0 splash, 1 title, 2 course select, 3 loading, 4 intro, 5 racing, 6 game over, 7 paused) |
| 14 | Tux speed, 8.8 m/s |

## Script language

```
run N                 run N frames
press BTN N           hold BTN for N frames, then release
hold BTN / release BTN
shot NAME             write NAME.ppm
comm                  print COMM0..14
expect REG V          assert COMM<REG> == V           (exit 5 on failure)
expectlo REG V        assert (COMM<REG> & 0xFF) == V
expectne REG V        assert COMM<REG> != V
waitlo REG V MAX      run up to MAX frames until (COMM<REG> & 0xFF) == V
```
Buttons: `up down left right a b c start x y z mode` (mapped to PicoDrive's
libretro ids; `00_pad.txt` proves the mapping against COMM8 every run).

## Scenarios

| script | what it proves |
|---|---|
| `00_pad` | every button reaches the SH-2 in the bit the game expects |
| `01_boot` | C code reached, video up, splash → title within 200 frames, title frames lit and animating |
| `02_menu` | title → Canadian Cup; cup course is locked to the current race |
| `03_gameplay` | cup → intro → race; speed telemetry non-zero; steering left/right and braking produce distinct, changing race frames; each race frame passes the snow/sky/Tux scene test |
| `04_pause_abort` | Start pauses (mode 7), A aborts to results (mode 6), Start returns to course select |
| `05_practice` | practice list browsing (course index in COMM12) and racing Frozen River |

Frame assertions (`run_tests.py`): ≥50 % lit pixels and ≥6 distinct colours
on every non-splash frame; at least two frames per script differ; frames
named `race*`/`intro*`/`paused`/`race_over`/`frozen*` must contain snow
(>10 %), sky (>5 %) and dark Tux pixels; consecutive `race_*` shots must
differ by ≥5 % of pixels.

## Running

```
make check            # clean release build + static verify + emulator suite + stamp
make test             # suite against the current rom/tuxracer.32x
make verified         # does rom/tuxracer.32x match the last passing stamp?
python3 tests/run_tests.py 03   # one script
TUXRACER_ROM=rom/tuxracer-bench.32x python3 tests/run_tests.py   # (fails, as it should)
```
Requires: 32XDK in `/opt/toolchains/sega`, `gcc`, `python3` (+ Pillow for
the contact sheet).  The suite runs in ~5 s.

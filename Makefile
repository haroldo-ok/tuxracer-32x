# Tux Racer 32X - build
#
# Requires the sh-elf / m68k-elf toolchains (Chilly Willy's Sega devkit,
# from viciious/32XDK releases) in /opt/toolchains/sega.
#
#   make            build rom/tuxracer.32x
#   make test       run the emulator (PicoDrive) black-screen/gameplay tests
#   make hosttest   build & run the host-side unit tests (fixed point, physics)

SEGA     ?= /opt/toolchains/sega
SHPREFIX ?= $(SEGA)/sh-elf/bin/sh-elf-
MDPREFIX ?= $(SEGA)/m68k-elf/bin/m68k-elf-

SHCC     := $(SHPREFIX)gcc
SHAS     := $(SHPREFIX)as
SHLD     := $(SHPREFIX)ld
SHOC     := $(SHPREFIX)objcopy
SHNM     := $(SHPREFIX)nm
MDAS     := $(MDPREFIX)as
MDLD     := $(MDPREFIX)ld
MDOC     := $(MDPREFIX)objcopy

# Debug/profiling builds (make bench / make prof) get their own object dir and
# ROM name so an instrumented image can never be published as rom/tuxracer.32x.
VARIANT  ?= release
OBJ      := obj/$(VARIANT)
ROMDIR   := rom
ifeq ($(VARIANT),release)
ROM      := $(ROMDIR)/tuxracer.32x
else
ROM      := $(ROMDIR)/tuxracer-$(VARIANT).32x
endif
ELF      := $(OBJ)/tuxracer.elf

# one optimisation level for every SH2 object (mixing -O levels bites on GCC 12 SH2)
SHCFLAGS := -m2 -mb -O2 -fomit-frame-pointer -fno-asynchronous-unwind-tables \
            -fno-unwind-tables -fno-builtin -nostdlib -D__32X__ \
            -Wall -Wextra -Wno-unused-parameter -Isrc/core -Isrc/platform/32x
ifeq ($(VARIANT),bench)
SHCFLAGS += -DBENCH $(BENCH_FLAGS)
endif
ifeq ($(VARIANT),prof)
SHCFLAGS += -DDEBUG_OVERLAY $(PROF_FLAGS)
endif
SHLIBGCC := $(shell $(SHCC) -m2 -mb -print-libgcc-file-name)

CORE_SRC := $(wildcard src/core/*.c)
GEN_SRC  := src/gen/tables.c src/gen/courses.c src/gen/tuxmodel.c src/gen/sounds.c
PLAT_SRC := src/platform/32x/main.c src/platform/32x/sound.c src/platform/32x/bench.c
CSRC     := $(CORE_SRC) $(GEN_SRC) $(PLAT_SRC)
COBJ     := $(patsubst src/%.c,$(OBJ)/%.o,$(CSRC))

all: $(ROM)

# instrumented variants: rom/tuxracer-bench.32x, rom/tuxracer-prof.32x
bench:
	$(MAKE) VARIANT=bench
prof:
	$(MAKE) VARIANT=prof

$(OBJ) $(ROMDIR):
	mkdir -p $@

# ------------------------------------------------------- generated data
src/gen/tables.c: tools/gen_tables.py
	python3 tools/gen_tables.py

src/gen/courses.c src/gen/tuxmodel.c: tools/convert_data.py
	python3 tools/convert_data.py

src/gen/sounds.c: tools/convert_sounds.py
	python3 tools/convert_sounds.py

src/core/font8.h: tools/gen_font.py
	python3 tools/gen_font.py

# ------------------------------------------------------------- 68000 side
$(OBJ)/m68k.o: src-md/m68k.s | $(OBJ)
	$(MDAS) -m68000 --register-prefix-optional -o $@ $<

$(OBJ)/m68k.bin: $(OBJ)/m68k.o src-md/m68k.ld
	$(MDLD) -T src-md/m68k.ld -o $(OBJ)/m68k.elf $<
	$(MDOC) -O binary $(OBJ)/m68k.elf $@

# --------------------------------------------------------------- SH2 side
$(OBJ)/crt0.o: src/platform/32x/crt0.s src/platform/32x/sega_startup.inc $(OBJ)/m68k.bin | $(OBJ)
	$(SHAS) --small -I src/platform/32x -I $(OBJ) -o $@ $<

$(OBJ)/%.o: src/%.c src/core/*.h src/platform/32x/*.h src/core/font8.h | $(OBJ)
	@mkdir -p $(dir $@)
	$(SHCC) $(SHCFLAGS) -c $< -o $@

$(OBJ)/raster.o: src/platform/32x/raster.s | $(OBJ)
	$(SHAS) --small -o $@ $<

$(ELF): $(OBJ)/crt0.o $(OBJ)/raster.o $(COBJ) ldscripts/mars.ld
	$(SHLD) -T ldscripts/mars.ld -Map $(OBJ)/tuxracer.map -o $@ $(OBJ)/crt0.o $(OBJ)/raster.o $(COBJ) $(SHLIBGCC)
	@$(SHNM) $@ | grep -E " (__bss_end|__data_start|__text_end)$$"

$(ROM): $(ELF) | $(ROMDIR)
	$(SHOC) -O binary $< $@
	python3 tools/fixrom.py $@
	@ls -l $@

# ------------------------------------------------------------ tests
# `make test` is the release gate: header/size checks, then the ROM is booted
# in PicoDrive and driven through title -> menu -> race with frame assertions
# (black-screen guard, mode telemetry, distinct checkpoints).
test: $(ROM) tests/harness
	python3 tests/verify_rom.py $(ROM) $(ELF) --title "TUX RACER 32X" \
	    --marker "Press Start to begin" --marker "Canadian Cup" --marker "Bunny Hill"
	python3 tests/run_tests.py
	python3 tests/contact_sheet.py

# build + test in one go (what CI / "is it shippable?" should run).
# On success rom/tuxracer.32x.verified records the sha256 of the ROM that
# passed; tools/verify_release.py refuses a ROM whose hash does not match.
check: clean-release all test
	python3 tools/stamp_verified.py $(ROM)

# quick "is the ROM in rom/ the one that passed the suite?" question
verified:
	python3 tools/stamp_verified.py --check $(ROM)

clean-release:
	rm -rf obj/release $(ROMDIR)/tuxracer.32x

HOSTOBJ  := obj/host
hosttest:
	mkdir -p $(HOSTOBJ)
	gcc -O1 -std=c11 -Isrc/core -o $(HOSTOBJ)/test_fixed tests/test_fixed.c src/core/fixed.c src/gen/tables.c -lm && $(HOSTOBJ)/test_fixed
	gcc -O1 -std=c11 -Isrc/core -w -o $(HOSTOBJ)/test_physics tests/test_physics.c src/core/fixed.c src/core/course.c \
	    src/core/tuxmodel.c src/core/physics.c src/core/view.c src/gen/tables.c src/gen/courses.c src/gen/tuxmodel.c -lm \
	    && $(HOSTOBJ)/test_physics

# emulator test harness (libretro frontend) - rebuilt when missing
tests/harness: tests/harness.c
	gcc -O2 -o $@ $< -ldl

clean:
	rm -rf obj $(ROMDIR)/*.32x

.PHONY: all clean clean-release test check verified hosttest bench prof

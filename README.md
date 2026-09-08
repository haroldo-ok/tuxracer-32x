# TuxRacer for 32X

This is a work-in-progress "vibe-port" experiment that ports TuxRacer to the Sega 32X hardware. The port is produced using Arena.ai in Agent mode and targets the 32X via the 32XDK toolchain.

Original source: https://tuxracer.sourceforge.net/

Skill used: https://github.com/haroldo-ok/sega-32x-skill-for-claude

Status

- WIP — this repository contains an experimental port and toolchain glue.
- Release ROMs are produced in rom/ and verified by an end-to-end emulator test suite (see tests/README.md).

Quick links

- Tests & verification: tests/README.md

Building

This project targets the Sega 32X and expects the 32XDK toolchain to be installed.

Requirements

- 32XDK installed at /opt/toolchains/sega
- gcc
- make
- python3 (+ Pillow for contact-sheet generation)

Common targets

- make                # build the project
- make check          # clean release build + static verify + emulator suite + stamp (release gate)
- make test           # run the emulator test suite against rom/tuxracer.32x
- make verified       # check whether rom/tuxracer.32x matches the last passing stamp

Running the emulator suite

The repository includes an automated end-to-end test harness that boots the built ROM in PicoDrive (libretro core), drives controller inputs, captures frames and 32X COMM telemetry, and asserts on both pixels and telemetry. See tests/README.md for details and for per-script usage examples.

Contributing

Contributions, bug reports and suggestions are welcome. If you want to help:

1. Open an issue describing the change or bug.
2. If you plan to contribute code, open a draft PR for discussion.

Credits

- Original TuxRacer: https://tuxracer.sourceforge.net/
- Project author / maintainer: haroldo-ok
- Tooling: Arena.ai (agent mode), sega-32x-skill-for-claude

License

See LICENSE if present in this repository.

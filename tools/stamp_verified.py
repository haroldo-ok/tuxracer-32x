#!/usr/bin/env python3
"""Record (or check) which ROM image passed the PicoDrive end-to-end suite.

    stamp_verified.py rom/tuxracer.32x          write rom/tuxracer.32x.verified
    stamp_verified.py --check rom/tuxracer.32x  exit 1 unless the hash matches

The stamp stores the sha256 + size + time so an instrumented/benchmark or
half-built ROM copied over rom/tuxracer.32x is detectable at a glance.
"""
import hashlib, os, sys, time

def sha(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 20), b''):
            h.update(chunk)
    return h.hexdigest()

def main(argv):
    check = '--check' in argv
    paths = [a for a in argv[1:] if not a.startswith('--')]
    if len(paths) != 1:
        raise SystemExit(__doc__)
    rom = paths[0]
    stamp = rom + '.verified'
    digest = sha(rom)
    if check:
        if not os.path.exists(stamp):
            raise SystemExit(f'NOT VERIFIED: {stamp} missing (run make check)')
        rec = dict(l.split('=', 1) for l in open(stamp).read().split('\n') if '=' in l)
        if rec.get('sha256') != digest:
            raise SystemExit(f'NOT VERIFIED: {rom} sha256 {digest[:16]}... != stamped {rec.get("sha256","?")[:16]}... '
                             f'(ROM changed since the suite passed; run make check)')
        print(f'verified: {rom} passed the PicoDrive suite at {rec.get("time")} ({rec.get("tests")} scripts)')
        return 0
    scripts = sorted(s for s in os.listdir(os.path.join(os.path.dirname(rom) or '.', '..', 'tests', 'scripts'))
                     if s.endswith('.txt')) if os.path.isdir(os.path.join(os.path.dirname(rom) or '.', '..', 'tests', 'scripts')) else []
    with open(stamp, 'w') as f:
        f.write(f'sha256={digest}\nsize={os.path.getsize(rom)}\n'
                f'time={time.strftime("%Y-%m-%d %H:%M:%S")}\ntests={len(scripts)}\n')
    print(f'stamped {stamp}: sha256 {digest[:16]}...')
    return 0

if __name__ == '__main__':
    raise SystemExit(main(sys.argv))

#!/usr/bin/env python3
"""Pad the ROM to a power-of-two size and fix the MD header checksum."""
import sys

path = sys.argv[1]
data = bytearray(open(path, 'rb').read())

# pad to next power of two, minimum 256 KB
size = 256 * 1024
while size < len(data):
    size *= 2
data += b'\xff' * (size - len(data))

# ROM end address in header
end = size - 1
data[0x1a4:0x1a8] = end.to_bytes(4, 'big')

# standard MD checksum: sum of words from 0x200
csum = 0
for i in range(0x200, len(data), 2):
    csum = (csum + (data[i] << 8) + data[i + 1]) & 0xffff
data[0x18e:0x190] = csum.to_bytes(2, 'big')

open(path, 'wb').write(data)
print('fixrom: %d bytes, checksum 0x%04X' % (len(data), csum))

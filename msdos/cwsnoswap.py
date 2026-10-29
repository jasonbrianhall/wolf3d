#!/usr/bin/env python3
"""Turn off CWSDPMI's swap file in CWSDPMI.EXE / CWSDSTUB.EXE.

This is the change CWSPARAM makes when the paging file name is set to ""
(the parameter block starts with "CWSPBLK\\0", the swap file name follows).
The game locks all of its memory, so paging can't help it; with a lot of
RAM (and under EMM386 / JEMM386, which rule out CWSDPMI's 4 MB pages)
CWSDPMI pages its own page tables out to C:\\CWSDPMI.SWP, and startup
crawls.
"""
import sys

for name in sys.argv[1:]:
    try:
        data = bytearray(open(name, 'rb').read())
    except OSError:
        continue
    i = data.find(b'CWSPBLK\x00')
    if i < 0:
        print(f'{name}: no CWSPBLK parameter block, left alone')
        continue
    if data[i + 8] == 0:
        continue                        # already done
    data[i + 8] = 0
    open(name, 'wb').write(data)
    print(f'{name}: swap file off')

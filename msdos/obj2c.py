#!/usr/bin/env python3
# Turns the data in one of Borland's OMF object files (obj/gamepal.obj,
# obj/signon.obj) into a C array, so DJGPP can link it.
#   python3 obj2c.py ../wolfsrc/obj/gamepal.obj gamepal > ../wolfsrc/gamepal.c
import sys

def index(b, p):
    v = b[p]
    if v & 0x80:
        return ((v & 0x7f) << 8) | b[p + 1], p + 2
    return v, p + 1

def omf_data(path):
    b = open(path, 'rb').read()
    p, data = 0, bytearray()
    while p < len(b):
        t = b[p]
        n = b[p + 1] | b[p + 2] << 8
        r = b[p + 3:p + 3 + n - 1]
        p += 3 + n
        if t == 0xA0:                       # LEDATA: segment, offset, bytes
            _, q = index(r, 0)
            off = r[q] | r[q + 1] << 8
            chunk = r[q + 2:]
            if len(data) < off + len(chunk):
                data.extend(bytes(off + len(chunk) - len(data)))
            data[off:off + len(chunk)] = chunk
    return data

path, name = sys.argv[1], sys.argv[2]
data = omf_data(path)
print("// %s: generated from %s by msdos/obj2c.py" % (name, path.split('/')[-1]))
print("unsigned char %s[%d] = {" % (name, len(data)))
for i in range(0, len(data), 20):
    print("\t" + ",".join(str(x) for x in data[i:i + 20]) + ",")
print("};")

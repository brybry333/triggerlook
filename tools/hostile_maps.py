#!/usr/bin/env python3
# hostile_maps.py <real map.bsp> <out dir>: writes crafted copies of a map that used to make triggerlook build gigabytes
# of shapes or hang at map load (a server can send any map). Run build/geomtest on each: it has to finish in about
# the time the real map takes, with no huge memory.
#   reuse.bsp  every brush one-sided, every brush model's tree = one leaf listing all brushes (each trigger re-reads them all)
#   inf.bsp    every trigger's "origin" and "angles" = inf (must be ignored, not turned into NaN shapes)
#   walk.bsp   the world's head node loops back to itself (front) or to leaf 0 (back, solid, listing every brush), every
#              brush side nodraw: each nodraw side's "faces open space" test and each sky face's area walk the loop
import lzma, os, re, struct, sys

src, out = sys.argv[1], sys.argv[2]
os.makedirs(out, exist_ok=True)
d = bytearray(open(src, 'rb').read())

def lump(i):
    ofs, ln, ver, unc = struct.unpack_from('<iiii', d, 8 + 16 * i)
    b = bytes(d[ofs:ofs + ln])
    if b[:4] == b'LZMA':
        actual, packed = struct.unpack_from('<II', b, 4); p = b[12:17]
        dec = lzma.LZMADecompressor(lzma.FORMAT_RAW, filters=[{'id': lzma.FILTER_LZMA1, 'dict_size': struct.unpack('<I', p[1:5])[0],
                                                                'lc': p[0] % 9, 'lp': (p[0] // 9) % 5, 'pb': p[0] // 45}])
        b = dec.decompress(b[17:17 + packed])[:actual]
    return b

def write(name, changes):
    # changed lumps go uncompressed at the end of the file
    m = bytearray(d)
    for i, data in changes.items():
        ofs = len(m)
        m += data
        struct.pack_into('<iiii', m, 8 + 16 * i, ofs, len(data), struct.unpack_from('<i', d, 8 + 16 * i + 8)[0], 0)
    open(os.path.join(out, name), 'wb').write(m)

TEXINFO, TEXDATA, NODES, LEAFS, MODELS, LEAFBRUSHES, BRUSHES, BRUSHSIDES, STRDATA, STRTABLE = 6, 2, 5, 10, 14, 17, 18, 19, 43, 44
leafs, models, brushes = bytearray(lump(LEAFS)), bytearray(lump(MODELS)), bytearray(lump(BRUSHES))
nb = min(len(brushes) // 12, 65535)
for b in range(len(brushes) // 12):
    struct.pack_into('<i', brushes, b * 12 + 4, 1)              # numsides = 1: no clip ever cuts it
struct.pack_into('<HH', leafs, 24, 0, nb)                       # leaf 0 lists every brush
for mi in range(1, len(models) // 48):
    struct.pack_into('<i', models, mi * 48 + 36, -1)            # headnode = leaf 0
write('reuse.bsp', {LEAFS: bytes(leafs), MODELS: bytes(models), BRUSHES: bytes(brushes),
                    LEAFBRUSHES: struct.pack('<%dH' % nb, *range(nb))})
print('wrote', os.path.join(out, 'reuse.bsp'))

# walk.bsp
nodes, leafs, sides = bytearray(lump(NODES)), bytearray(lump(LEAFS)), bytearray(lump(BRUSHSIDES))
texinfo, texdata, strdata, strtable = lump(TEXINFO), lump(TEXDATA), lump(STRDATA), lump(STRTABLE)
def texname(ti):
    td = struct.unpack_from('<i', texinfo, ti * 72 + 68)[0]
    so = struct.unpack_from('<i', strtable, struct.unpack_from('<i', texdata, td * 32 + 12)[0] * 4)[0]
    return strdata[so:strdata.index(b'\0', so)].decode('latin1').upper()
nodraw = next(ti for ti in range(len(texinfo) // 72) if texname(ti).endswith('TOOLSNODRAW'))
for s_ in range(len(sides) // 8):
    struct.pack_into('<h', sides, s_ * 8 + 2, nodraw)
head = struct.unpack_from('<i', lump(MODELS), 36)[0]
struct.pack_into('<ii', nodes, head * 32 + 4, head, -1)        # front: itself, back: leaf 0
struct.pack_into('<i', leafs, 0, 1)                             # leaf 0 solid: every corner of every side is tried
struct.pack_into('<HH', leafs, 24, 0, nb)
write('walk.bsp', {NODES: bytes(nodes), LEAFS: bytes(leafs), BRUSHSIDES: bytes(sides), LEAFBRUSHES: struct.pack('<%dH' % nb, *range(nb))})
print('wrote', os.path.join(out, 'walk.bsp'))

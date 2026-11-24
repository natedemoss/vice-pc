"""Read-only PE inspector: header, sections, per-section entropy, imports, signs of encryption."""
import math, struct, sys
from collections import Counter

def entropy(b):
    if not b: return 0.0
    c = Counter(b); n = len(b)
    return -sum(v/n*math.log2(v/n) for v in c.values())

path = sys.argv[1]
with open(path, 'rb') as f:
    data = f.read()

print(f"size: {len(data):,} bytes")
print("first 16 bytes:", data[:16].hex(' '))
if data[:2] != b'MZ':
    print("NOT a PE (no MZ). whole-file sample entropy:", round(entropy(data[:1<<20]), 3))
    sys.exit()
pe = struct.unpack_from('<I', data, 0x3C)[0]
if data[pe:pe+4] != b'PE\0\0':
    print("MZ but no PE signature"); sys.exit()
machine, nsec, ts, _, _, optsz, chars = struct.unpack_from('<HHIIIHH', data, pe+4)
print(f"machine=0x{machine:04x} sections={nsec} timestamp=0x{ts:08x} characteristics=0x{chars:04x}")
opt = pe + 24
magic = struct.unpack_from('<H', data, opt)[0]
print(f"optional magic=0x{magic:x} ({'PE32+' if magic==0x20b else 'PE32'})")
entry = struct.unpack_from('<I', data, opt+16)[0]
imagebase = struct.unpack_from('<Q', data, opt+24)[0]
subsys = struct.unpack_from('<H', data, opt+68)[0]
print(f"entry RVA=0x{entry:x} imagebase=0x{imagebase:x} subsystem={subsys}")
ndd = struct.unpack_from('<I', data, opt+108)[0]
dd = [struct.unpack_from('<II', data, opt+112+8*i) for i in range(min(ndd,16))]
names = ['export','import','resource','exception','security','reloc','debug','arch','globalptr','tls','loadcfg','boundimp','iat','delayimp','clr','res']
for i,(rva,sz) in enumerate(dd):
    if sz: print(f"  dir {names[i]:9s} rva=0x{rva:x} size=0x{sz:x}")
sec_off = opt + optsz
secs = []
print("sections:")
for i in range(nsec):
    o = sec_off + 40*i
    name = data[o:o+8].rstrip(b'\0').decode('latin1')
    vsz, va, rsz, rptr = struct.unpack_from('<IIII', data, o+8)
    sch = struct.unpack_from('<I', data, o+36)[0]
    raw = data[rptr:rptr+rsz]
    secs.append((name, va, vsz, rptr, rsz))
    print(f"  {name:10s} va=0x{va:08x} vsz=0x{vsz:08x} raw=0x{rsz:08x} chars=0x{sch:08x} entropy={entropy(raw[:4<<20]):.3f}")

def rva2off(rva):
    for n, va, vsz, rptr, rsz in secs:
        if va <= rva < va + max(vsz, rsz):
            return rptr + (rva - va)
    return None

# entry point bytes
eo = rva2off(entry)
if eo is not None:
    print("entry bytes:", data[eo:eo+32].hex(' '))

# imports
irva, isz = dd[1] if len(dd) > 1 else (0,0)
if irva:
    o = rva2off(irva)
    print("imported DLLs:")
    while o is not None:
        oft, _, _, nrva, ft = struct.unpack_from('<IIIII', data, o)
        if not nrva: break
        no = rva2off(nrva)
        dll = data[no:data.index(b'\0', no)].decode('latin1') if no else '?'
        fnames = []
        to = rva2off(oft or ft)
        while to is not None:
            v = struct.unpack_from('<Q', data, to)[0]
            if not v: break
            if v >> 63:
                fnames.append(f"#{v & 0xffff}")
            else:
                ho = rva2off(v & 0x7fffffff)
                fnames.append(data[ho+2:data.index(b'\0', ho+2)].decode('latin1'))
            to += 8
        print(f"  {dll} ({len(fnames)} funcs)")
        want = [w.lower() for w in sys.argv[2:]]
        if 'all' in want or dll.lower() in want:
            for n in fnames: print(f"      {n}")
        o += 20

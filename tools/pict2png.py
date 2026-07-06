#!/usr/bin/env python3
"""Decode classic 1-bit QuickDraw PICT (v1) resources to PNG.

Handles the opcodes a black-and-white game PICT uses: version, clip, the pen/
text/pattern state ops (skipped), and the bitmap ops BitsRect (0x90) and
PackBitsRect (0x98) which carry the actual image. Unknown/unsupported opcodes
stop that picture (with a note) rather than crash.

Usage:  python pict2png.py work/rsrc -o work/pict_png
"""
import argparse, glob, os, struct
from PIL import Image

def u16(b,o): return struct.unpack_from('>H',b,o)[0]
def s16(b,o): return struct.unpack_from('>h',b,o)[0]

def unpackbits(row, expected):
    """MacPaint PackBits decompression -> `expected` bytes."""
    out=bytearray(); i=0
    while len(out)<expected and i<len(row):
        n=row[i]; i+=1
        if n<128: out += row[i:i+n+1]; i+=n+1
        else:     out += bytes([row[i]])*(257-n); i+=1
    return bytes(out[:expected])

def decode_bitmap(b, o, packed):
    rowbytes = u16(b,o); o+=2
    top=s16(b,o); left=s16(b,o+2); bottom=s16(b,o+4); right=s16(b,o+6); o+=8
    o+=8+8+2                                   # srcRect, dstRect, mode
    w=right-left; h=bottom-top
    if rowbytes & 0x8000:                       # pixmap (color) - not for these
        return None,o
    img=Image.new('1',(w,h))
    px=img.load()
    for y in range(h):
        if packed:
            ln = b[o] if rowbytes<=250 else u16(b,o); o += 1 if rowbytes<=250 else 2
            raw=unpackbits(b[o:o+ln], rowbytes); o+=ln
        else:
            raw=b[o:o+rowbytes]; o+=rowbytes
        for x in range(w):
            bit=(raw[x>>3]>>(7-(x&7)))&1 if (x>>3)<len(raw) else 0
            px[x,y]= 0 if bit else 1            # QuickDraw: 1 = black
    return img,o

# minimal PICT v1 opcode sizes (bytes of data after the 1-byte opcode)
FIXED={0x00:0,0x02:8,0x03:2,0x04:1,0x05:2,0x06:4,0x07:4,0x08:2,0x09:8,0x0a:8,
       0x0b:4,0x0c:4,0x0d:2,0x0e:4,0x0f:4,0x10:8,0x11:1,0x15:2,0x16:2,
       0x20:8,0x21:4,0x22:6,0x23:2,0x30:8,0x31:8,0x32:8,0x33:8,0x34:8,0x35:8,0x36:8,0x37:8,
       0x38:0,0x39:0,0x3a:0,0x3b:0,0x3c:0,0x3d:0,0x3e:0,0x3f:0,
       0x50:8,0x51:8,0x52:8,0x53:8,0x54:8,0x55:8,0x56:8,0x57:8,
       0x58:0,0x59:0,0x5a:0,0x5b:0,0x5c:0,0x5d:0,0x5e:0,0x5f:0,0xa0:2}

def decode(data):
    o=2                                         # skip picSize
    o+=8                                         # frame rect
    imgs=[]
    while o < len(data):
        op=data[o]; o+=1
        if op==0xff: break
        if op in (0x90,0x91,0x98,0x99):
            img,o=decode_bitmap(data,o,packed=op in (0x98,0x99))
            if op in (0x91,0x99):                # region variant: skip trailing rgn (rare here)
                pass
            if img: imgs.append(img)
        elif op==0x01:                           # clipRgn
            rgnsz=u16(data,o); o+=rgnsz
        elif op==0xa1:                           # long comment
            o+=2; sz=u16(data,o); o+=2+sz
        elif op in FIXED:
            o+=FIXED[op]
        elif 0x28<=op<=0x2b:                      # text ops: bail (uncommon in art)
            return imgs,"text-op 0x%02x"%op
        else:
            return imgs,"unknown op 0x%02x at %d"%(op,o-1)
    return imgs,None

def main():
    ap=argparse.ArgumentParser(); ap.add_argument("rsrc_dir"); ap.add_argument("-o","--out",default="pict_png")
    a=ap.parse_args(); os.makedirs(a.out,exist_ok=True)
    ok=fail=0
    for f in sorted(glob.glob(os.path.join(a.rsrc_dir,"PICT_*.bin"))):
        data=open(f,"rb").read(); name=os.path.splitext(os.path.basename(f))[0]
        try:
            imgs,err=decode(data)
        except Exception as e:
            imgs,err=[],repr(e)
        if imgs:
            for i,im in enumerate(imgs):
                im.save(os.path.join(a.out,f"{name}{'' if len(imgs)==1 else '_'+str(i)}.png"))
            ok+=1
        else:
            fail+=1
            if fail<=8: print(f"  {name}: {err}")
    print(f"decoded {ok} PICTs, {fail} not decoded -> {a.out}/")

if __name__=="__main__":
    main()

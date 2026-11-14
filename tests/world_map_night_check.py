"""Check in-place night texture composition and preservation without game resources."""
from pathlib import Path
import base64
import hashlib
import struct
import sys

sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
from world_map_night import apply_texture_patch

b=bytearray(0x460)
b[:4]=b'BNTX'
struct.pack_into('<IQ',b,0x24,2,0x80)
patches=[];changed=set()
for i in range(2):
    info,name_at,mips,data=0x100+0xa0*i,0x300+0x40*i,0x380+8*i,0x400+0x20*i
    struct.pack_into('<Q',b,0x80+8*i,info);b[info:info+4]=b'BRTI'
    name='test'+str(i);struct.pack_into('<H',b,name_at,len(name));b[name_at+2:name_at+2+len(name)]=name.encode()
    struct.pack_into('<Q',b,info+0x60,name_at);struct.pack_into('<H',b,info+0x16,1)
    struct.pack_into('<I',b,info+0x1c,0x1d01);struct.pack_into('<II',b,info+0x24,4,4)
    struct.pack_into('<I',b,info+0x50,32);struct.pack_into('<Q',b,info+0x70,mips);struct.pack_into('<Q',b,mips,data)
    b[data:data+32]=bytes([10+i])*32
    properties=bytes(b[info+0xc:info+0x60]);pixels=bytes(b[data:data+32])
    new=bytearray(properties);struct.pack_into('<I',new,0x1c-0xc,0x2d06)
    patches.append(dict(name=name,old=hashlib.sha256(properties+pixels).hexdigest(),
                        properties=base64.b64encode(new).decode(),pixels=base64.b64encode(bytes([20+i])*32).decode()))
    changed.update(range(info+0xc,info+0x60));changed.update(range(data,data+32))
original=bytes(b);result=apply_texture_patch(original,patches)
assert len(result)==len(original) and original==b
assert all(result[i]==original[i] for i in range(len(b)) if i not in changed)
assert apply_texture_patch(result,patches)==result
bad=[dict(patches[0],old='0'*64),patches[1]]
try:apply_texture_patch(original,bad)
except ValueError:pass
else:raise AssertionError('changed original accepted')
assert original==b
print('world_map_night_check: passed (composition, fixed allocation, idempotence, unrelated bytes, original guard)')

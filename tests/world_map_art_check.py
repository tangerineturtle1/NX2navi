"""Resource-free checks for duplicate UI archive variants and untouched member preservation."""
from pathlib import Path
import struct
import sys

sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
from world_map_art import archive_nodes,archive_replace,collectible_layout

items=[('collect',b'first variant'),('other',b'untouched'),('collect',b'second variant'),('other',b'also untouched')]
names=bytearray()
offsets=[]
for name,_ in items:
    offsets.append(len(names)//4)
    names.extend(name.encode()+b'\0')
    names.extend(bytes(-len(names)%4))
start=(20+12+16*len(items)+8+len(names)+31)//32*32
b=bytearray(start)
struct.pack_into('<4sHHIIHH',b,0,b'SARC',20,0xfeff,0,start,0x100,0)
struct.pack_into('<4sHHI',b,20,b'SFAT',12,len(items),0x65)
sfnt=32+16*len(items)
struct.pack_into('<4sHH',b,sfnt,b'SFNT',8,0)
b[sfnt+8:sfnt+8+len(names)]=names
for i,((name,body),offset) in enumerate(zip(items,offsets)):
    b.extend(bytes(-len(b)%16))
    struct.pack_into('<4I',b,32+16*i,i,0x1000000|offset,len(b)-start,len(b)-start+len(body))
    b.extend(body)
struct.pack_into('<I',b,8,len(b))
out=archive_replace(bytes(b),'collect',lambda body:body+b' extended')
nodes=archive_nodes(out)
assert len(nodes)==len(items)
for (name,_,lo,hi),(expected,body) in zip(nodes,items):
    assert name==expected
    assert out[lo:hi]==body+(b' extended' if name=='collect' else b'')
    assert lo%16==0
assert struct.unpack_from('<I',out,8)[0]==len(out)
for action in [lambda:archive_replace(bytes(b),'missing',bytes),lambda:collectible_layout(b'invalid')]:
    try:action()
    except ValueError:pass
    else:raise AssertionError('unsupported resource accepted')
print('world_map_art_check: passed (duplicate variants, unrelated members, alignment, unsupported inputs)')

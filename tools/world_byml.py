"""Typed BYML editing for locally generated map overlays; preserves native scalar types."""
import struct


class Scalar:
    def __init__(self,kind,value):self.kind,self.value=kind,value
    def __int__(self):return int(self.value)
    def __float__(self):return float(self.value)
    def __bool__(self):return bool(self.value)


def read(blob):
    if blob[:2]!=b'YB':raise ValueError('expected little-endian map BYML')
    def u32(at):return struct.unpack_from('<I',blob,at)[0]
    def u24(at):return int.from_bytes(blob[at:at+3],'little')
    def strings(at):
        if not at:return []
        return [blob[at+u32(at+4+4*i):].split(b'\0',1)[0].decode() for i in range(u24(at+1))]
    keys,values=strings(u32(4)),strings(u32(8))
    def scalar(kind,value):
        if kind==0xa0:return values[value]
        if kind in (0xc0,0xc1):return node(value)
        formats={0xd0:'I',0xd1:'i',0xd2:'f',0xd3:'I',0xd4:'q',0xd5:'Q',0xd6:'d'}
        if kind==0xff:return None
        if kind not in formats:raise ValueError(f'unsupported map scalar {kind:#x}')
        raw=blob[value:value+8] if kind in (0xd4,0xd5,0xd6) else struct.pack('<I',value)
        return Scalar(kind,struct.unpack('<'+formats[kind],raw)[0])
    def node(at):
        kind,count=blob[at],u24(at+1)
        if kind==0xc1:return {keys[u24(at+4+8*i)]:scalar(blob[at+7+8*i],u32(at+8+8*i)) for i in range(count)}
        if kind==0xc0:
            start=(at+4+count+3)&~3
            return [scalar(blob[at+4+i],u32(start+4*i)) for i in range(count)]
        raise ValueError('unsupported map container')
    return node(u32(12)),struct.unpack_from('<H',blob,2)[0]


def write(root,version):
    keys=set();strings=set()
    def gather(value):
        if isinstance(value,dict):
            keys.update(value)
            for child in value.values():gather(child)
        elif isinstance(value,list):
            for child in value:gather(child)
        elif isinstance(value,str):strings.add(value)
    gather(root);keys=sorted(keys);strings=sorted(strings)
    key_index={value:i for i,value in enumerate(keys)};string_index={value:i for i,value in enumerate(strings)}
    out=bytearray(b'YB'+struct.pack('<HIII',version,0,0,0))
    def align(size=4):out.extend(bytes(-len(out)%size))
    def table(values):
        align();start=len(out);out.extend(b'\xc2'+len(values).to_bytes(3,'little'))
        out.extend(bytes(4*(len(values)+1)))
        for i,value in enumerate(values):
            struct.pack_into('<I',out,start+4+4*i,len(out)-start);out.extend(value.encode()+b'\0')
        struct.pack_into('<I',out,start+4+4*len(values),len(out)-start)
        return start
    struct.pack_into('<II',out,4,table(keys),table(strings))
    def scalar(value):
        if value is None:return 0xff,0
        if isinstance(value,str):return 0xa0,string_index[value]
        if isinstance(value,(dict,list)):return (0xc1 if isinstance(value,dict) else 0xc0),node(value)
        if not isinstance(value,Scalar):
            value=Scalar(0xd0,int(value)) if isinstance(value,bool) else Scalar(0xd2,value) if isinstance(value,float) else Scalar(0xd5 if value>0xffffffff else 0xd1 if value<0 else 0xd3,value)
        formats={0xd0:'I',0xd1:'i',0xd2:'f',0xd3:'I',0xd4:'q',0xd5:'Q',0xd6:'d'}
        raw=struct.pack('<'+formats[value.kind],value.value)
        if len(raw)==8:
            align(8);at=len(out);out.extend(raw);return value.kind,at
        return value.kind,struct.unpack('<I',raw)[0]
    def node(value):
        align();start=len(out);count=len(value)
        if isinstance(value,dict):
            out.extend(b'\xc1'+count.to_bytes(3,'little')+bytes(count*8))
            for i,(key,child) in enumerate(sorted(value.items(),key=lambda pair:key_index[pair[0]])):
                kind,word=scalar(child);at=start+4+i*8
                out[at:at+3]=key_index[key].to_bytes(3,'little');out[at+3]=kind;struct.pack_into('<I',out,at+4,word)
        else:
            out.extend(b'\xc0'+count.to_bytes(3,'little')+bytes(count));align();words=len(out);out.extend(bytes(count*4))
            for i,child in enumerate(value):
                kind,word=scalar(child);out[start+4+i]=kind;struct.pack_into('<I',out,words+4*i,word)
        return start
    at=node(root);struct.pack_into('<I',out,12,at)
    return bytes(out)

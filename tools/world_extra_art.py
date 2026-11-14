"""Extend the player's facility layout and generate optional world icon variants locally."""
import base64
import ctypes
import hashlib
import struct

SHADOWS=('MapIconShopMaskSh_00','MapIconTrainingSh_00','MapIconShopBombchuSh_00','MapIconMiniGameDampeSh_01',
         'MapIconMiniGameFrogSh_00','MapIconMiniGameArcherySh_00','MapIconMiniGameCoccoSh_00','MapIconMiniGameEponaSh_00')
MEMBER='PaMapIconFacility_00.buil'


def layout(original):
    b=bytearray(original);u32=lambda at:struct.unpack_from('<I',b,at)[0];u64=lambda at:struct.unpack_from('<Q',b,at)[0]
    pool=u32(0xd4);controller=0x1a10
    if b[:4]!=b'BFFH' or u32(8)!=len(b) or u32(controller+0x5c)!=38:raise ValueError('facility layout changed')
    extra=bytearray()
    def append(data):
        extra.extend(bytes(-(pool+len(extra))%8));at=pool+len(extra);extra.extend(data);return at
    arrays=u64(controller+24);changes=[]
    if u32(controller+32)!=7:raise ValueError('facility animation channels changed')
    for i in range(7):
        channel=u64(arrays+i*8);knot,curve=struct.unpack_from('<QQ',b,channel)
        kp,kn,ks=struct.unpack_from('<QII',b,knot+8);vp,vn,vs=struct.unpack_from('<QII',b,curve+8)
        if ks!=4 or vs!=4 or vn!=kn*2:raise ValueError('facility curve encoding changed')
        times=list(struct.unpack_from('<'+'f'*kn,b,kp));values=list(struct.unpack_from('<'+'f'*vn,b,vp))
        prototype=(50.,50.,1.,0.,0.,8.,8.)[i]
        if times[-1]<38:times.append(38.);values.extend((values[-2],0.))
        for frame in range(39,47):
            selected=31.+frame-39 if i in (5,6) else prototype
            times.extend((float(frame),float(frame)));values.extend((values[-2],0.,selected,0.))
        changes.append((knot,curve,append(struct.pack('<'+'f'*len(times),*times)),len(times),
                        append(struct.pack('<'+'f'*len(values),*values)),len(values)))
    banks=[]
    for at in (0x808,0x978):
        ptr,count,stride=struct.unpack_from('<QII',b,at)
        if count!=31 or stride!=8:raise ValueError('facility texture banks changed')
        banks.append((at,struct.unpack_from('<31Q',b,ptr),append(bytes(39*8))))
    extra.extend(bytes(-len(extra)%8));delta=len(extra);strings=original[pool:];names=[];addition=bytearray()
    for name in SHADOWS:names.append(pool+delta+len(strings)+len(addition));addition.extend(name.encode()+b'\0')
    for at in range(0xf8,pool,8):
        pointer=u64(at)
        if pool<=pointer<len(original):struct.pack_into('<Q',b,at,pointer+delta)
    b=b[:pool]+extra+strings+addition
    struct.pack_into('<I',b,8,len(b));struct.pack_into('<I',b,0xb0,u32(0xb0)+delta)
    struct.pack_into('<II',b,0xd4,pool+delta,len(strings)+len(addition));struct.pack_into('<Q',b,0xe0,pool+delta)
    for knot,curve,kp,kn,vp,vn in changes:
        struct.pack_into('<f',b,knot+4,46.);struct.pack_into('<QII',b,knot+8,kp,kn,4);struct.pack_into('<QII',b,curve+8,vp,vn,4)
    for at,old,ptr in banks:
        shifted=[value+delta if pool<=value<len(original) else value for value in old]
        struct.pack_into('<39Q',b,ptr,*(shifted+names));struct.pack_into('<QII',b,at,ptr,39,8)
    struct.pack_into('<I',b,controller+0x5c,46)
    return bytes(b)


def textures(original,cache,compiler=None,env=None):
    from PIL import Image,ImageDraw
    from pad_prompts import bntx_textures,read_level,write_level,astc_encode,bleed
    from ui_textures import astc_library
    decoder=astc_library(cache,compiler=compiler,env=env);entries=bntx_textures(original)
    count,table=struct.unpack_from('<IQ',original,0x24);infos={}
    for i in range(count):
        at=struct.unpack_from('<Q',original,table+i*8)[0];p=struct.unpack_from('<Q',original,at+0x60)[0]
        name=original[p+2:p+2+struct.unpack_from('<H',original,p)[0]].decode();infos[name]=at
    template=original[infos['MapIconCollectSkullSign_00']+0xc:infos['MapIconCollectSkullSign_00']+0x60]
    images=[]
    cave=Image.new('RGBA',(256,256));draw=ImageDraw.Draw(cave)
    draw.polygon([(14,225),(22,99),(70,40),(128,18),(191,46),(237,113),(244,225)],fill=(121,81,43,255),outline=(38,27,16,255),width=10)
    draw.ellipse((69,65,197,237),fill=(38,29,24,255));draw.rectangle((68,150,197,237),fill=(38,29,24,255));draw.line((18,229,240,229),fill=(251,218,150,255),width=12)
    images.append(cave)
    for name in ('MapIconMagicBean_00','MapIconGreatFairy_00'):
        t=entries[name]
        if t['fmt']!=0x2d06 or t['mips']!=1:raise ValueError('world icon source format changed')
        raw=read_level(original,t,((t['w']+3)//4)*16,(t['h']+3)//4);rgba=ctypes.create_string_buffer(t['w']*t['h']*4)
        decoder.astc_decode_ldr(bytes(raw),t['w'],t['h'],4,4,True,rgba)
        source=Image.frombytes('RGBA',(t['w'],t['h']),rgba.raw)
        bounds=source.getchannel('A').point(lambda a:255 if a>16 else 0).getbbox();source=source.crop(bounds)
        scale=220/max(source.size);source=source.resize((round(source.width*scale),round(source.height*scale)),Image.Resampling.LANCZOS)
        image=Image.new('RGBA',(256,256));image.alpha_composite(source,((256-source.width)//2,(256-source.height)//2));images.append(image)
    upgrade=Image.new('RGBA',(256,256));draw=ImageDraw.Draw(upgrade)
    draw.rounded_rectangle((26,77,227,236),radius=32,fill=(112,75,36,255),outline=(37,28,14,255),width=10)
    draw.polygon([(128,10),(233,113),(177,113),(177,203),(79,203),(79,113),(23,113)],fill=(130,229,123,255),outline=(26,51,25,255),width=11)
    images.append(upgrade);patches=[]
    for i,target in enumerate(SHADOWS):
        t=entries[target]
        if t['fmt']!=0x1d01 or t['size']<4096:raise ValueError('world icon destination allocation changed')
        image=images[i//2].copy()
        if i%2:
            draw=ImageDraw.Draw(image);draw.ellipse((151,1,254,104),fill=(48,31,16,255))
            draw.line([(169,52),(191,75),(237,25)],fill=(255,255,239,255),width=17)
        image=image.resize((64,64),Image.Resampling.LANCZOS)
        props=bytearray(template);struct.pack_into('<II',props,0x24-0xc,64,64);struct.pack_into('<I',props,0x34-0xc,1)
        struct.pack_into('<I',props,0x50-0xc,t['size']);struct.pack_into('<I',props,0x54-0xc,struct.unpack_from('<I',original,infos[target]+0x54)[0])
        pixels=bytearray(t['size']);write_level(pixels,dict(start=0,size=t['size'],th=1),astc_encode(bleed(image)),256,16)
        old=original[infos[target]+0xc:infos[target]+0x60]+original[t['start']:t['start']+t['size']]
        patches.append(dict(name=target,old=hashlib.sha256(old).hexdigest(),properties=base64.b64encode(props).decode(),pixels=base64.b64encode(pixels).decode()))
        image.save(cache/f'world-extra-{i}.png')
    return patches

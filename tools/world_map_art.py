"""Extend the player's map collectible layout with unchecked icons already present in its texture archive.

No game resources are stored here. Output belongs in an ignored build or game overlay directory.
"""
import struct
from pathlib import Path

ARCHIVE = 'UI/Layout2Archive/Common.Product.100.Nin_Ounce_NVN2.blarc.zs'
MEMBER = 'PaMapIconCollect_00.buil'


def collectible_layout(original):
    """Retain checked states 0/1; add unchecked 2/3 and nocturnal skull states 4/5."""
    b = bytearray(original)
    u32 = lambda at: struct.unpack_from('<I', b, at)[0]
    u64 = lambda at: struct.unpack_from('<Q', b, at)[0]
    if b[:4] != b'BFFH' or b[0xf8:0xfc] != b'UIL\0' or u32(8) != len(b):
        raise ValueError('unrecognized collectible map layout')
    pool = u32(0xd4)
    if pool != 0xf80 or u32(0xd8) != len(b)-pool:
        raise ValueError('collectible map layout sections changed')
    expected = {0x580: (0x5b0,2,8), 0x608: (0x638,2,8)}
    for at, value in expected.items():
        if struct.unpack_from('<QII', b, at) != value:
            raise ValueError('collectible texture banks changed')
    if u32(0xcc4) != 1:
        raise ValueError('collectible controller duration changed')
    extra = bytearray()
    def append(data):
        extra.extend(bytes(-(pool+len(extra)) % 8))
        at = pool+len(extra);extra.extend(data);return at
    # Native step curves repeat each transition time, with a value/tangent pair at every knot.
    changes = []
    for knot, curve in [(0xd40,0xd68),(0xdb0,0xdd8)]:
        if struct.unpack_from('<ff', b, knot) != (0.,1.) or struct.unpack_from('<ff', b, curve) != (1.,0.):
            raise ValueError('collectible animation domain changed')
        if u32(knot+16) != 3 or u32(curve+16) != 6:
            raise ValueError('collectible animation curves changed')
        times = append(struct.pack('<11f',0,1,1,2,2,3,3,4,4,5,5))
        values = append(struct.pack('<22f',0,0,0,0,1,0,1,0,2,0,2,0,3,0,3,0,4,0,4,0,5,0))
        changes.extend([(knot,times,11),(curve,values,22)])
    shadow = append(bytes(48));icon = append(bytes(48))
    extra.extend(bytes(-len(extra) % 8))
    delta = len(extra)
    strings = original[pool:]
    additions = b'MapIconCollectHeartPiece_00\0MapIconCollectSkullSign_00\0'
    heart = pool+delta+len(strings)
    skull = heart+len(b'MapIconCollectHeartPiece_00\0')
    moon=heart+len(additions)
    moon_checked=moon+len(b'MapIconCollectHeartPieceCheckSh_00\0')
    additions+=b'MapIconCollectHeartPieceCheckSh_00\0MapIconCollectSkullSignCheckSh_00\0'
    # All serialized pointers are absolute file offsets. Existing object and vector addresses stay put;
    # the original string pool moves to make room for the extra vectors in the Default section.
    for at in range(0xf8,pool,8):
        value=u64(at)
        if pool <= value < len(original):struct.pack_into('<Q',b,at,value+delta)
    b = b[:pool]+extra+strings+additions
    struct.pack_into('<I',b,8,len(b))
    struct.pack_into('<I',b,0xb0,u32(0xb0)+delta)
    struct.pack_into('<I',b,0xd4,pool+delta)
    struct.pack_into('<I',b,0xd8,len(strings)+len(additions))
    struct.pack_into('<Q',b,0xe0,pool+delta)
    for at,ptr,count in changes:
        if at in (0xd40,0xdb0): struct.pack_into('<ff',b,at,0,5)
        struct.pack_into('<QII',b,at+8,ptr,count,4)
    # The native shadow material supplies its own dark tint; an unchecked icon's alpha gives its matching shadow.
    struct.pack_into('<6Q',b,shadow,0x1005+delta,0x1026+delta,heart,skull,moon,moon_checked)
    struct.pack_into('<6Q',b,icon,0x1005+delta,0x1026+delta,heart,skull,moon,moon_checked)
    struct.pack_into('<QII',b,0x580,shadow,6,8)
    struct.pack_into('<QII',b,0x608,icon,6,8)
    struct.pack_into('<I',b,0xcc4,5)
    return bytes(b)


def archive_nodes(data):
    """Keep every node: this archive contains several variants with the same member name."""
    if data[:4] != b'SARC':
        raise ValueError('unrecognized UI archive')
    header = struct.unpack_from('<H',data,4)[0]
    count = struct.unpack_from('<H',data,header+6)[0]
    start = struct.unpack_from('<I',data,12)[0]
    names = header+12+16*count+8
    result=[]
    for i in range(count):
        node=header+12+16*i
        _,attribute,lo,hi=struct.unpack_from('<4I',data,node)
        name_at=names+(attribute&0xffffff)*4
        name=data[name_at:data.index(0,name_at)].decode()
        result.append((name,node,start+lo,start+hi))
    return result


def archive_replace(data, member, transform, preprocess=lambda body:body):
    nodes = archive_nodes(data)
    if not any(name==member for name,_,_,_ in nodes):
        raise ValueError('collectible layout missing')
    start = struct.unpack_from('<I', data, 12)[0]
    out = bytearray(data[:start])
    for name,node,lo,hi in sorted(nodes, key=lambda item: item[2]):
        alignment = min(lo & -lo, 0x2000) if lo else 0x2000
        out.extend(bytes(-len(out) % alignment))
        body=preprocess(data[lo:hi])
        if name==member:body=transform(body)
        struct.pack_into('<II', out, node+8, len(out)-start, len(out)-start+len(body))
        out.extend(body)
    struct.pack_into('<I', out, 8, len(out))
    return bytes(out)


def prepare(game,compiler=None,env=None):
    """Generate locally from the player's game; the original archive is never changed."""
    import hashlib
    import json
    from pad_prompts import Romfs, compress, write_overlay
    import world_map_night as night
    import world_extra,world_extra_art,world_byml
    game = Path(game)
    destination = game/'romfs-patches-world'
    stamp = destination/'generator.sha256'
    romfs = Romfs(game/'romfs')
    if ARCHIVE not in romfs.paths():
        return  # This layout belongs only to the Switch 2 game.
    original = romfs.raw(ARCHIVE)
    original_textures=romfs.raw(night.TEXTURES)
    key = hashlib.sha256(Path(__file__).read_bytes()+Path(night.__file__).read_bytes()+
        Path(world_extra.__file__).read_bytes()+Path(world_extra_art.__file__).read_bytes()+Path(world_byml.__file__).read_bytes()+
        hashlib.sha256(original).digest()+hashlib.sha256(original_textures).digest()+
        hashlib.sha256(romfs.raw(world_extra.MAP)).digest()).hexdigest()
    required=['manifest.tsv',ARCHIVE,night.TEXTURES,world_extra.MAP,'extra-world.bin','skull-night.bin','night-texture-patch.json','night-icons.stamp']
    if stamp.is_file() and stamp.read_text() == key and all((destination/name).is_file() for name in required):
        return
    archive, dictionary = romfs.load(ARCHIVE)
    def ordinary_shadows(body):
        # Reuse the two dedicated checked-shadow textures for night variants; ordinary shadows use the main alpha.
        for name in night.SHADOWS+world_extra_art.SHADOWS:
            old=name.encode()+b'\0';new=name.replace('Sh_','_').encode()+b'\0'
            body=body.replace(old,new+bytes(len(old)-len(new)))
        return body
    result = archive_replace(archive, MEMBER, collectible_layout,ordinary_shadows)
    result = archive_replace(result,world_extra_art.MEMBER,world_extra_art.layout)
    textures,texture_dictionary=romfs.load(night.TEXTURES)
    patches=night.texture_patch(textures,game/'build-cache/world-art',compiler=compiler,env=env)
    patches+=world_extra_art.textures(textures,game/'build-cache/world-art',compiler=compiler,env=env)
    changed=night.apply_texture_patch(textures,patches)
    outputs={ARCHIVE:compress(result,dictionary,9),night.TEXTURES:compress(changed,texture_dictionary,9)}
    map_data,map_dictionary,metadata,report=world_extra.build(romfs)
    outputs[world_extra.MAP]=compress(map_data,map_dictionary,9)
    write_overlay(destination,outputs,{ARCHIVE:original,night.TEXTURES:original_textures,world_extra.MAP:romfs.raw(world_extra.MAP)})
    (destination/'extra-world.bin').write_bytes(metadata)
    (destination/'extra-world-report.json').write_text(json.dumps(report,indent=2))
    ids=night.night_ids(romfs)
    (destination/'skull-night.bin').write_bytes(b'NXWNT001'+struct.pack('<I',len(ids))+struct.pack('<'+'Q'*len(ids),*ids))
    (destination/'night-texture-patch.json').write_text(json.dumps(patches))
    (destination/'night-icons.stamp').write_text(hashlib.sha256(outputs[night.TEXTURES]).hexdigest())
    stamp.write_text(key)
    print('  collectible map icons prepared from the player\'s game', flush=True)


if __name__ == '__main__':
    import sys
    prepare(Path(sys.argv[1]))

"""Derive optional world markers from the player's map, placements and reward flags."""
import hashlib
import re
import struct
from pathlib import PurePosixPath
from world_byml import Scalar,read,write
from world_map_night import byml

MAP='Banc/BancBasedResource/MapParam.byml.zs'
GROTTOS,BEANS,FAIRIES,UPGRADES=1,2,3,4


def murmur(name):
    data=name.encode();h=0
    def rot(v,n):return ((v<<n)|(v>>(32-n)))&0xffffffff
    for i in range(0,len(data)//4*4,4):
        k=int.from_bytes(data[i:i+4],'little');k=k*0xcc9e2d51&0xffffffff;k=rot(k,15);k=k*0x1b873593&0xffffffff
        h^=k;h=rot(h,13);h=(h*5+0xe6546b64)&0xffffffff
    k=int.from_bytes(data[len(data)//4*4:],'little')
    if len(data)%4:k=k*0xcc9e2d51&0xffffffff;k=rot(k,15);k=k*0x1b873593&0xffffffff;h^=k
    h^=len(data);h^=h>>16;h=h*0x85ebca6b&0xffffffff;h^=h>>13;h=h*0xc2b2ae35&0xffffffff
    return (h^(h>>16))&0xffffffff


def build(romfs):
    paths=set(romfs.paths());original,dictionary=romfs.load(MAP)
    data,version=read(original)
    flags=byml(romfs.load('GameData/GameDataList.Product.110.byml.zs')[0])['Data']['Bool']
    indexes={row['Hash']:i for i,row in enumerate(flags)}
    records={};report=[];placements={area:[] for area in data};groups={area:[] for area in data}
    def condition(names):
        result=[]
        for name in names:
            h=murmur(name)
            if h not in indexes:return []
            result.append((h,indexes[h]))
        return result
    def add(area,category,source,position,names=()):
        identity=int.from_bytes(hashlib.sha256(f'NX2navi world {area} {category} {source}'.encode()).digest()[:8],'little')
        if identity in records:return
        tests=condition(names)
        if len(tests)>2:raise ValueError('too many world marker conditions')
        records[identity]=(category,0,tests)
        data[area].setdefault('Facility',[]).append(dict(InstanceID=Scalar(0xd5,identity),IconType=Scalar(0xd1,11),
            Trans=[Scalar(0xd2,float(v)) for v in position],GameDataHashName='',
            HeartPieceHash0=Scalar(0xd5,0),HeartPieceHash1=Scalar(0xd5,0)))
        report.append(dict(area=area,category=category,id=identity,source=str(source),position=position,
                           completion=list(names) if tests else []))
    for path in sorted(paths):
        if not (path.startswith('Banc/') and path.endswith('.bcett.byml.zs')):continue
        stem=PurePosixPath(path).name.removesuffix('.bcett.byml.zs')
        area=re.sub(r'_(Static|Dynamic)$','',stem)
        if area not in data or 'Floor' in data[area]:continue
        value=byml(romfs.load(path)[0])
        placements[area].extend(value.get('Actors',[]));groups[area].extend(value.get('AiGroups',[]))
    reward_flags={20:('MiniGame_HitTarget_HrlCity_Get_BulletBag',),21:('MiniGame_HitTarget_Kakariko_Get_Quiver',),
                  7:('MiniGame_HorseBackArchery_Get_Quiver',),
                  24:('KakarikoVillage_GatheringTokenGold_GetReward10','KakarikoVillage_GatheringTokenGold_GetReward30')}
    for area,row in data.items():
        for facility in list(row.get('Facility',[])):
            icon=int(facility['IconType']);position=[float(v) for v in facility['Trans']]
            if icon==11:add(area,FAIRIES,int(facility['InstanceID']),position,(facility['GameDataHashName'],))
            upgrade=icon in (7,20,21,22,24,27) or (icon==28 and '_Adult' in area) or (icon==33 and '_Child' in area)
            if upgrade:add(area,UPGRADES,int(facility['InstanceID']),position,reward_flags.get(icon,()))
        actors=placements[area];by_id={actor['Hash']:actor for actor in actors}
        chests={int(chest['InstanceID']) for chest in row.get('TBox',[])}
        for actor in actors:
            kind=actor.get('Gyaml','');identity=actor['Hash'];position=actor.get('Translate')
            if not position:continue
            if kind=='LookAttentionActor_HidingHoleEntrance':add(area,GROTTOS,identity,position)
            # Soil's DiveInsectGameData records released bugs, not whether a bean was planted.
            if kind=='Obj_SmallFarm_A_01':add(area,BEANS,identity,position)
            drop=actor.get('Dynamic',{}).get('Drop__DropActor','')
            if kind.startswith(('Obj_TBox_','TBox_')) and identity not in chests:
                row.setdefault('TBox',[]).append(dict(InstanceID=Scalar(0xd5,identity),Trans=[Scalar(0xd2,float(v)) for v in position]));chests.add(identity)
                records[identity]=(UPGRADES,records.get(identity,(UPGRADES,0,[]))[1]|2,[])
            capacity=any(part in str(drop) for part in ['Bag_Medium','Bag_Large','Quiver_Medium','Quiver_Large','Wallet_Medium','Wallet_Large'])
            if capacity and kind.startswith(('Obj_TBox_','TBox_')):records[identity]=(UPGRADES,records.get(identity,(UPGRADES,0,[]))[1]|1,[])
            if capacity and 'Scrub' in kind:add(area,UPGRADES,identity,position)
        # The rolling Goron's bag reward is a scene-logic reference, not a map facility.
        for group in groups[area]:
            path='Logic/'+PurePosixPath(group['Logic']).name+'b'
            if 'GoronCity_Child_Prefab' not in path or path not in paths:continue
            blob,_=romfs.load(path)
            if b'GoronCity_GetBombBag_fromGoron006\0' not in blob:continue
            for ref in group.get('References',[]):
                actor=by_id.get(ref.get('Reference'))
                if actor and actor.get('Gyaml','').startswith('Npc_Goron_006'):
                    add(area,UPGRADES,actor['Hash'],actor['Translate'],('GoronCity_GetBombBag_fromGoron006',))
    payload=bytearray(b'NXWEX001'+struct.pack('<I',len(records)))
    for identity,(category,flags,tests) in sorted(records.items()):
        pairs=tests+[(0,0)]*(2-len(tests))
        payload.extend(struct.pack('<Q8I',identity,category,flags,len(tests),pairs[0][0],pairs[0][1],pairs[1][0],pairs[1][1],0))
    return write(data,version),dictionary,bytes(payload),report

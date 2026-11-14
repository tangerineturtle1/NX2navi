"""Preserve scalar kinds and nested map structures through local BYML generation."""
from pathlib import Path
import sys,math
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
from world_byml import Scalar,read,write

def same(a,b):
    if isinstance(a,Scalar):
        assert isinstance(b,Scalar) and a.kind==b.kind
        assert a.value==b.value or (isinstance(a.value,float) and math.isnan(a.value) and math.isnan(b.value))
    elif isinstance(a,dict):
        assert a.keys()==b.keys()
        for key in a:same(a[key],b[key])
    elif isinstance(a,list):
        assert len(a)==len(b)
        for x,y in zip(a,b):same(x,y)
    else:assert a==b

fixture={'area':{'Facility':[
    {'InstanceID':Scalar(0xd5,0),'Trans':[Scalar(0xd2,-0.0),Scalar(0xd2,1.25),Scalar(0xd2,-30.5)],'Name':'fairy'},
    {'InstanceID':Scalar(0xd5,2**64-1),'Name':'bean'}],
    'types':[Scalar(0xd0,1),Scalar(0xd1,-2147483648),Scalar(0xd3,2**32-1),
             Scalar(0xd4,-2**63),Scalar(0xd6,1.125),None,'',[],{}]},'unicode':'moon \u263e'}
for version in (2,7):
    encoded=write(fixture,version);decoded,actual=read(encoded)
    assert actual==version;same(fixture,decoded)
    assert write(decoded,actual)==encoded
print('world_byml_check: passed (native scalar types, signed limits, 64-bit IDs, nested containers, determinism)')

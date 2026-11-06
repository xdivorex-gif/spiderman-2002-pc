"""Research decoder for USM torso, limb IK and finger scalar channels.
Uses Claude's x86 integer emulator and the user's own USM.exe.
Output is channel-space data; limb IK and rig application are still required.
"""
import argparse
import json
import math
from pathlib import Path
import struct
import sys
from usm_pack import Pack
from usm_channels import animation_channels
from usm_entropy import initial_scalars

WIDTHS = ((3,3,3,3,3,6), (3,3,7,7), (3,3,7,7), (3,3)+(2,)*8+(1,)*10)


def quaternion(xyz):
    return list(xyz) + [math.sqrt(abs(1-sum(x*x for x in xyz)))]


def multiply(a,b):
    x,y,z,w=a; X,Y,Z,W=b
    return [w*X+x*W+y*Z-z*Y,w*Y+y*W+z*X-x*Z,w*Z+z*W+x*Y-y*X,w*W-x*X-y*Y-z*Z]


def decode_channel(record, index, CPU, Mem, executable_word):
    channels=animation_channels(record,7)
    metadata=channels[index]['metadata']; start=channels[index]['stream']
    end=min([c['stream'] for c in channels if c['stream'] is not None and c['stream']>start]+[len(record)])
    stream=record[start:end]
    mask=struct.unpack_from('<I',record,metadata)[0]
    widths=WIDTHS[index]
    if mask >> len(widths): raise ValueError('unsupported channel mask')
    groups=[(i,w) for i,w in enumerate(widths) if mask & (1<<i)]
    count=sum(w for i,w in groups)
    descriptors=record[metadata+4:metadata+4+count]
    scale=struct.unpack_from('<f',record,0x54)[0]
    wide=bool(struct.unpack_from('<I',record,0x34)[0]&0x20000)
    first=initial_scalars(stream,descriptors,scale,wide)
    values=first['initial'][:]; velocity=first['velocity'][:]
    mem=Mem(); mem.write(0x100000,stream+bytes(8))
    mem.w32(0x200000,0x100000+first['bits_consumed']//8)
    mem.write(0x200004,bytes([first['bits_consumed']%8,255,0,0]))
    cpu=CPU(mem); pending=[0]*count
    frames=[values[:]]; frame_count=struct.unpack_from('<I',record,0x50)[0]
    for frame in range(1,frame_count):
        if frame>=2:
            for i,descriptor in enumerate(descriptors):
                mode=descriptor&63; delta=0
                if mode:
                    if pending[i]: pending[i]-=1
                    else:
                        run=cpu.call(executable_word(0x937250+mode*4),[0x200000,0x300000])
                        if not 0<run<65536: raise ValueError('invalid entropy run')
                        pending[i]=run-1; delta=mem.u32(0x300000)
                        if delta & 0x80000000: delta-=2**32
                velocity[i]+=delta*scale/1024
        cursor=0
        for group,width in groups:
            # Torso and limb groups each begin with a quaternion; finger groups
            # 0/1 are quaternions, remaining groups are scalar curl parameters.
            rotation=index<3 or group<2
            if rotation:
                q=quaternion(values[cursor:cursor+3])
                dq=quaternion(velocity[cursor:cursor+3])
                length=math.sqrt(sum(x*x for x in dq)); dq=[x/length for x in dq]
                q=multiply(dq,q)
                values[cursor:cursor+3]=[x if q[3]>=0 else -x for x in q[:3]]
            for i in range(cursor+(3 if rotation else 0),cursor+width):values[i]+=velocity[i]
            cursor+=width
        frames.append(values[:])
        bit=(mem.u32(0x200000)-0x100000)*8+mem.read(0x200004,1)[0]
        if bit>len(stream)*8:raise ValueError(f'channel {index} frame {frame}: stream overrun')
        if not all(math.isfinite(v) for v in values):raise ValueError('nonfinite channel value')
    bit=(mem.u32(0x200000)-0x100000)*8+mem.read(0x200004,1)[0]
    return dict(index=index,mask=mask,groups=groups,frames=frames,bits_consumed=bit,stream_bits=len(stream)*8)


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--pack',required=True,type=Path)
    ap.add_argument('--research',required=True,type=Path)
    ap.add_argument('--output',required=True,type=Path)
    ap.add_argument('--names',nargs='*',default=['venidl','venrun'])
    args=ap.parse_args();sys.path.insert(0,str(args.research))
    from emu import CPU,Mem
    from exe import u32
    args.output.mkdir(parents=True,exist_ok=True)
    for name,record in Pack(args.pack).animations():
        if args.names and name not in args.names:continue
        decoded=[decode_channel(record,i,CPU,Mem,u32) for i in range(4)]
        report=dict(name=name,status='channel-space research output; not bone animation',channels=decoded)
        (args.output/(name+'-channels.json')).write_text(json.dumps(report,indent=2))
        print(name,[(c['index'],len(c['frames']),c['bits_consumed'],c['stream_bits']) for c in decoded],flush=True)

if __name__=='__main__': main()

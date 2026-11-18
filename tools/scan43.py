import struct,glob,os
for f in sorted(glob.glob('F:/Claude/OB-12/rom/dsp/prog*.pmem.bin')):
    b=open(f,'rb').read(); n=len(b)//4
    be=[struct.unpack('>I',b[i*4:i*4+4])[0] for i in range(n)]
    print(os.path.basename(f),n)
    for i,w in enumerate(be):
        p=w>>24
        if p in (0x43,0x47,0x4b,0x4f):
            prev=None
            for j in range(i-1,max(i-12,-1),-1):
                q=be[j]>>24
                if 0x40<=q<=0x4f and q not in (0x43,0x47,0x4b,0x4f) or 0x6c<=q<=0x71:
                    prev=(j,be[j]);break
            print('  %03X %08X sec %02X | before %s | last C mult %s'%(i,w,(w>>16)&0x3f if 0 else (w>>18)&0x3f,' '.join('%08X'%x for x in be[max(i-2,0):i]),'%03X %08X d=%d'%(prev[0],prev[1],i-prev[0]) if prev else None))

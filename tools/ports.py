# DASP4 output ports from build/ports.raw between two times: level, pitch, flatness per half frame.
import sys,numpy as np
path,t0,t1=sys.argv[1],float(sys.argv[2]),float(sys.argv[3])
x=np.memmap(path,dtype='<i4',mode='r').reshape(-1,6)
names=['AX1L','AX1R','MOUTL','MOUTR','AOUTL','AOUTR']
r=48000
for s in (0,1):
    y=np.array(x[int(t0*96000)+s:int(t1*96000):2],dtype=float)
    # keep the slot fixed: start index parity
    for c,n in enumerate(names):
        v=y[:,c]; m=v.mean(); v=v-m
        if v.std()<1: print(s,n,'const %.0f'%m); continue
        f=np.abs(np.fft.rfft(v*np.hanning(len(v)))); fr=np.fft.rfftfreq(len(v),1/r)
        idx=[i for i in range(2,len(f)-1) if f[i]>f[i-1] and f[i]>f[i+1] and f[i]>f.max()*0.1][:6]
        print(s,n,'mean %.0f rms %.0f peak %d flat %.3f'%(m,v.std(),np.abs(y[:,c]).max(),np.exp(np.mean(np.log(f[1:]+1)))/np.mean(f[1:])),[(round(float(fr[i]),1),round(float(f[i]/f.max()),2)) for i in idx])

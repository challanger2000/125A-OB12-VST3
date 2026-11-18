# Periodicity of each logged cell at a given pitch: how much of the signal repeats every period.
import numpy as np,sys
a=np.fromfile('build/cells.bin',dtype='<i4').reshape(-1,768)
hz=float(sys.argv[1]); names=sys.argv[2:] 
n=len(a); P=48000/hz
rows=[]
for c in range(768):
    x=a[:,c].astype(float); x=x-x.mean()
    if x.std()<500: continue
    best=0
    for k in (1,2,3,4):
        L=int(round(P*k))
        for l in (L-1,L,L+1):
            u,v=x[:-l],x[l:]
            r=(u*v).sum()/np.sqrt((u*u).sum()*(v*v).sum()+1)
            best=max(best,r)
    f=np.abs(np.fft.rfft(x*np.hanning(n)))**2; fr=np.fft.rfftfreq(n,1/48000)
    rows.append((best,('D%03X'%c) if c<256 else ('C%03X'%(c-256)),x.std(),f[fr>4000].sum()/f.sum()))
rows.sort(key=lambda r:r[1])
for r in rows:
    if r[1].startswith('D') : print('%s period-corr %.3f rms %9.0f hi %.3f'%(r[1],r[0],r[2],r[3]))

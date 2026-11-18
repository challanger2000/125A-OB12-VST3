import numpy as np,sys
a=np.fromfile('build/cells.bin',dtype='<i4').reshape(-1,768)
print(a.shape)
n=len(a)
rows=[]
for c in range(768):
    x=a[:,c].astype(float); 
    r=x.std()
    if r<16: continue
    y=x-x.mean(); f=np.abs(np.fft.rfft(y*np.hanning(n))); fr=np.fft.rfftfreq(n,1/48000)
    i=int(f[1:].argmax())+1
    rows.append((r,('D%03X'%c) if c<256 else ('C%03X'%(c-256)),fr[i],f[i]/f.sum(),int(x.min()),int(x.max())))
rows.sort(reverse=True)
for r in rows[:int(sys.argv[1]) if len(sys.argv)>1 else 70]:
    print('%s rms %9.0f peak %8.1f Hz conc %.3f min %9d max %9d'%(r[1],r[0],r[2],r[3],r[4],r[5]))

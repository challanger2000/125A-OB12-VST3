# Peaks, flatness and level of a stereo WAV between two times.
import sys,wave,numpy as np
w=wave.open(sys.argv[1]); r=w.getframerate(); t0,t1=float(sys.argv[2]),float(sys.argv[3])
x=np.frombuffer(w.readframes(w.getnframes()),dtype='<i2').reshape(-1,2).astype(float)
print('rate',r,'peak',int(np.abs(x).max()))
for c,n in ((0,'L'),(1,'R')):
    y=x[int(t0*r):int(t1*r),c]; print(n,'mean %.1f rms %.1f'%(y.mean(),y.std()),end=' ')
    y=y-y.mean()
    if y.std()<1: print('silent'); continue
    f=np.abs(np.fft.rfft(y*np.hanning(len(y)))); fr=np.fft.rfftfreq(len(y),1/r)
    idx=[i for i in range(2,len(f)-1) if f[i]>f[i-1] and f[i]>f[i+1] and f[i]>f.max()*0.05][:10]
    print('flat %.3f'%(np.exp(np.mean(np.log(f[1:]+1)))/np.mean(f[1:])),[(round(float(fr[i]),1),round(float(f[i]/f.max()),2)) for i in idx])

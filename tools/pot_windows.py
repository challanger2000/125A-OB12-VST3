# Level, flatness and strongest lines of MOUT left in fixed windows of build/ports.raw.
import sys,numpy as np
x=np.memmap(sys.argv[1],dtype='<i4',mode='r').reshape(-1,6)
for a,b in zip(sys.argv[2::2],sys.argv[3::2]):
    a,b=float(a),float(b)
    v=np.array(x[int(a*96000):int(b*96000):2,2],float);v-=v.mean()
    f=np.abs(np.fft.rfft(v*np.hanning(len(v))));fr=np.fft.rfftfreq(len(v),1/48000)
    i=np.argsort(f)[-3:][::-1]
    print('%.1f-%.1f rms %.0f flat %.3f'%(a,b,v.std(),np.exp(np.mean(np.log(f[1:]+1)))/np.mean(f[1:])),[round(float(fr[k])) for k in i])

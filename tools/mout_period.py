# Is MOUT left periodic? Best autocorrelation between lags, and pitch per 50 ms frame.
import sys,numpy as np
x=np.memmap(sys.argv[1],dtype='<i4',mode='r').reshape(-1,6)
a,b=float(sys.argv[2]),float(sys.argv[3])
v=np.array(x[int(a*96000):int(b*96000):2,2],float);v-=v.mean()
def ac(y,lo=48,hi=1600):
    n=len(y);f=np.fft.rfft(y,2*n);c=np.fft.irfft(f*np.conj(f))[:hi]
    c/=c[0]; k=lo+int(np.argmax(c[lo:hi])); return 48000/k,c[k]
print('whole: pitch %.2f Hz corr %.3f'%ac(v))
fr=[ac(v[i:i+4800]) for i in range(0,len(v)-4800,2400)]
print('frames:',' '.join('%.0f/%.2f'%q for q in fr[:14]))

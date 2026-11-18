import numpy as np,sys
t0=float(sys.argv[1]); t1=float(sys.argv[2]); wav=sys.argv[3] if len(sys.argv)>3 else None
b=np.fromfile('build/voice_bus.raw',dtype='<i4').reshape(-1,2)
s=b[1::2]; print('odd slot secs',len(s)/48000)
# envelope
for a in np.arange(t0-0.3,t1,0.25):
    x=s[int(a*48000):int((a+.25)*48000)].astype(float)
    if len(x): print('%.2f rmsL %8.0f rmsR %8.0f meanL %8.0f'%(a,x[:,0].std(),x[:,1].std(),x[:,0].mean()))
for c,n in ((0,'L'),(1,'R')):
    x=s[int(t0*48000):int(t1*48000),c].astype(float); x-=x.mean()
    if x.std()<50: print(n,'silent'); continue
    N=len(x); f=np.abs(np.fft.rfft(x*np.hanning(N))); fr=np.fft.rfftfreq(N,1/48000)
    idx=[i for i in range(2,len(f)-1) if f[i]>f[i-1] and f[i]>f[i+1] and f[i]>f.max()*0.05][:12]
    ac=np.fft.irfft(np.abs(np.fft.rfft(x,2*N))**2)[:N]; lo=48000//2000; hi=48000//40
    lag=lo+int(ac[lo:hi].argmax())
    p=f**2; flat=np.exp(np.mean(np.log(f[1:]+1)))/np.mean(f[1:])
    print(n,'autocorr %.2f Hz (%.3f)'%(48000/lag,ac[lag]/ac[0]),'flat %.3f'%flat,[(round(float(fr[i]),1),round(float(f[i]/f.max()),2)) for i in idx])
if wav:
    import wave
    a=int((t0-0.5)*48000); y=s[a:a+48000*4].astype(float)/256
    y=np.clip(y,-32768,32767).astype('<i2')
    w=wave.open(wav,'wb'); w.setnchannels(2); w.setsampwidth(2); w.setframerate(48000); w.writeframes(y.tobytes()); w.close()

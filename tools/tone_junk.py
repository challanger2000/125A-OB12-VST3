# Level and everything but the tone (400-480 Hz) per second and channel, for OB12_AUX_TONE runs.
# usage: tone_junk.py out.wav ...
import wave, numpy as np, sys
for p in sys.argv[1:]:
    w=wave.open(p); a=np.frombuffer(w.readframes(w.getnframes()),'<i2').astype(float).reshape(-1,2)/32768
    print(p)
    for t in (16,17,18):
        out=[]
        for ch in (0,1):
            y=a[int(t*48000):int((t+1)*48000),ch]; f=np.abs(np.fft.rfft(y*np.hanning(len(y))))**2; fr=np.fft.rfftfreq(len(y),1/48000)
            near=(fr>400)&(fr<480); tot=f.sum()+1e-30
            top=np.argsort(f*(~near))[-3:]
            out.append("rms %.4f pk %.3f junk %.1f dB [%s]"%(y.std(),np.abs(y).max(),10*np.log10((f[~near].sum()+1e-30)/tot)," ".join("%d"%fr[k] for k in sorted(top))))
        print("  %d L %s | R %s"%(t,out[0],out[1]))

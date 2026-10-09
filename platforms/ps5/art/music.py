import numpy as np, wave
SR=48000; BPM=132; beat=60/BPM; bars=16; dur=bars*4*beat
N=int(dur*SR); t=np.arange(N)/SR
L=np.zeros(N,np.float32); R=np.zeros(N,np.float32)
def note(f): return 440*2**((f-69)/12)
def env(n,a=0.005,d=0.15,s=0.6,r=0.1,length=None):
    length=length or n; x=np.arange(n)/SR; e=np.ones(n)
    e=np.minimum(e, x/a if a>0 else 1)
    e=np.where(x>a, s+(1-s)*np.exp(-(x-a)/d), e)
    rel=np.clip((length/SR - x)/r,0,1); return e*rel
def add(buf,start,sig,pan=0.0,gain=1.0):
    i=int(start*SR); j=min(N,i+len(sig)); s=sig[:j-i]*gain
    L[i:j]+=s*(1-pan)*0.7071*1.2; R[i:j]+=s*(1+pan)*0.7071*1.2
def saw(f,n,det=0.0):
    x=np.arange(n)/SR; out=0
    for d in (-det,0,det): out=out+2*((x*f*(1+d))%1)-1
    return out/3
def sq(f,n,duty=0.5):
    x=np.arange(n)/SR; return np.where((x*f)%1<duty,1.0,-1.0)
def tri(f,n):
    x=np.arange(n)/SR; return 2*np.abs(2*((x*f)%1)-1)-1
# progression (original): Am F C G | Am F G E  repeated
prog=[(57,[0,3,7]),(53,[0,4,7]),(48,[0,4,7]),(55,[0,4,7]),(57,[0,3,7]),(53,[0,4,7]),(55,[0,4,7]),(52,[0,4,7])]
for bar in range(bars):
    root,ch=prog[bar%8]; t0=bar*4*beat
    # pad
    n=int(4*beat*SR)
    for k,iv in enumerate(ch):
        sig=saw(note(root+12+iv),n,0.004)*env(n,0.08,1.0,0.7,0.3)
        add(L,t0,sig,pan=(k-1)*0.5,gain=0.07)
    # bass 8ths
    for e in range(8):
        n=int(beat/2*SR); f=note(root-12+(12 if e%2 else 0))
        add(L,t0+e*beat/2,(sq(f,n,0.3)*0.6+tri(f,n))*env(n,0.002,0.08,0.4,0.03),gain=0.16)
    # arpeggio 16ths after bar 4
    if bar>=4:
        pat=[0,1,2,1,0,2,1,2]
        for s in range(16):
            n=int(beat/4*SR); iv=ch[pat[s%8]]+(12 if s>=8 else 0)
            add(L,t0+s*beat/4,sq(note(root+24+iv),n,0.25)*env(n,0.002,0.05,0.2,0.02),pan=0.35*np.sin(s),gain=0.05)
    # lead melody bars 8-15 (original motif)
    if bar>=8:
        mel=[(0,7,1),(1,9,0.5),(1.5,12,0.5),(2,11,1),(3,7,1)] if bar%2==0 else [(0,4,1.5),(1.5,5,0.5),(2,7,2)]
        for (b0,iv,ln) in mel:
            n=int(ln*beat*SR); f=note(root+24+iv)
            x=np.arange(n)/SR; vib=np.sin(2*np.pi*5.5*x)*0.004
            sig=(2*((np.cumsum(f*(1+vib))/SR)%1)-1)*0.5+sq(f,n,0.5)*0.3
            add(L,t0+b0*beat,sig*env(n,0.01,0.3,0.6,0.08),pan=-0.1,gain=0.09)
    # drums from bar 2
    if bar>=2:
        for b in range(4):
            n=int(0.25*SR); x=np.arange(n)/SR
            kick=np.sin(2*np.pi*np.cumsum(50+120*np.exp(-x*30))/SR)*np.exp(-x*12)
            add(L,t0+b*beat,kick,gain=0.5)
            if b in (1,3):
                n2=int(0.2*SR); sn=np.random.default_rng(bar*4+b).standard_normal(n2)*np.exp(-np.arange(n2)/SR*22)
                add(L,t0+b*beat,sn,gain=0.18)
            for h in (0,1):
                n3=int(0.05*SR); hh=np.random.default_rng(1000+bar*8+b*2+h).standard_normal(n3)
                hh=np.diff(np.concatenate([[0],hh]))*np.exp(-np.arange(n3)/SR*80)
                add(L,t0+b*beat+h*beat/2,hh,pan=0.3,gain=0.08)
# simple stereo delay
d=int(beat*0.75*SR); L[d:]+=R[:-d]*0.18; R[d:]+=L[:-d]*0.18
m=max(np.abs(L).max(),np.abs(R).max()); L/=m*1.12; R/=m*1.12
# fade edges for clean loop
f=int(0.01*SR); L[:f]*=np.linspace(0,1,f); R[:f]*=np.linspace(0,1,f); L[-f:]*=np.linspace(1,0,f); R[-f:]*=np.linspace(1,0,f)
pcm=(np.stack([L,R],1)*32767).astype('<i2')
with wave.open('snd0-source.wav','wb') as w:
    w.setnchannels(2); w.setsampwidth(2); w.setframerate(SR); w.writeframes(pcm.tobytes())
print(dur)

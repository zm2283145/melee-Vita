import numpy as np, math, random
from PIL import Image, ImageDraw, ImageFilter, ImageFont, ImageChops
W,H=3840,2160
rng=np.random.default_rng(7); random.seed(7)

def lerp(a,b,t): return a+(b-a)*t
def fbm(w,h,octaves=6,base=4,seed=0):
    r=np.random.default_rng(seed); out=np.zeros((h,w),np.float32); amp=1; tot=0
    for o in range(octaves):
        gw,gh=base*2**o+1,int(base*2**o*h/w)+2
        g=r.random((gh,gw)).astype(np.float32)
        img=Image.fromarray((g*255).astype(np.uint8)).resize((w,h),Image.BICUBIC)
        out+=np.asarray(img,np.float32)/255*amp; tot+=amp; amp*=0.5
    return out/tot

def sky():
    y=np.linspace(0,1,H)[:,None]
    stops=[(0.0,(10,8,40)),(0.35,(38,20,92)),(0.58,(120,40,120)),(0.72,(230,90,90)),(0.80,(255,170,90)),(1.0,(40,20,60))]
    img=np.zeros((H,W,3),np.float32)
    for i in range(len(stops)-1):
        y0,c0=stops[i]; y1,c1=stops[i+1]
        m=((y>=y0)&(y<=y1)).astype(np.float32); t=np.clip((y-y0)/(y1-y0),0,1)
        for c in range(3): img[:,:,c]+=m[:,0:1]*lerp(c0[c],c1[c],t)[:,0:1]*np.ones((1,W))
    return img

def add_glow(img,cx,cy,r,col,strength):
    yy,xx=np.mgrid[0:H,0:W].astype(np.float32)
    d=np.sqrt((xx-cx)**2+(yy-cy)**2)/r
    g=np.exp(-d*d)*strength
    for c in range(3): img[:,:,c]+=g*col[c]
    return img

img=sky()
# stars in upper sky
for _ in range(1800):
    x=random.randrange(W); y=int(random.random()**1.8*H*0.55); b=random.random()
    s=1 if b<0.85 else 2
    img[y:y+s,x:x+s,:]+=np.array([200,210,255])*b*(1-y/(H*0.55))
# sun + halo
sx,sy=W*0.66,H*0.70
img=add_glow(img,sx,sy,900,(255,120,60),0.55)
img=add_glow(img,sx,sy,260,(255,200,120),1.2)
yy,xx=np.mgrid[0:H,0:W]
sun=((xx-sx)**2+(yy-sy)**2)<210**2
img[sun]=img[sun]*0.2+np.array([255,236,190])*0.8
# light rays from sun
ang=np.arctan2(yy-sy,xx-sx); rays=(np.sin(ang*18)*0.5+0.5)**6
dist=np.sqrt((xx-sx)**2+(yy-sy)**2); fall=np.clip(1-dist/2600,0,1)**2
for c,v in enumerate((255,170,110)): img[:,:,c]+=rays*fall*v*0.35
# cloud bands
for i,(yc,thick,col,alpha,seed) in enumerate([(0.62,0.10,(120,60,130),0.55,1),(0.78,0.12,(255,140,110),0.5,2),(0.88,0.10,(70,30,80),0.85,3)]):
    n=fbm(W,H,6,5,seed)
    band=np.exp(-((np.linspace(0,1,H)[:,None]-yc)/thick)**2)
    m=np.clip((n-0.45)*3.2,0,1)*band*alpha
    for c in range(3): img[:,:,c]=img[:,:,c]*(1-m)+col[c]*m
img=np.clip(img,0,255).astype(np.uint8)
base=Image.fromarray(img,'RGB')

# floating arena platform silhouette with rim light
plat=Image.new('RGBA',(W,H),(0,0,0,0)); d=ImageDraw.Draw(plat)
cx,top=W*0.5,H*0.80; half=1250
d.polygon([(cx-half,top),(cx+half,top),(cx+half-160,top+70),(cx+300,top+320),(cx,top+430),(cx-300,top+320),(cx-half+160,top+70)],fill=(16,10,30,255))
# small side platforms
for ox,oy,hw in [(-880,-260,260),(520,-470,380)]:
    d.rounded_rectangle([cx+ox-hw,top+oy,cx+ox+hw,top+oy+34],radius=16,fill=(20,14,38,255))
rim=Image.new('RGBA',(W,H),(0,0,0,0)); dr=ImageDraw.Draw(rim)
dr.line([(cx-half,top),(cx+half,top)],fill=(255,190,120,255),width=10)
for ox,oy,hw in [(-880,-260,260),(520,-470,380)]:
    dr.line([(cx+ox-hw+12,top+oy),(cx+ox+hw-12,top+oy)],fill=(255,200,140,255),width=8)
glow=rim.filter(ImageFilter.GaussianBlur(28))
base=base.convert('RGBA'); base.alpha_composite(plat); base.alpha_composite(glow); base.alpha_composite(glow); base.alpha_composite(rim)
# sparks
sp=Image.new('RGBA',(W,H),(0,0,0,0)); ds=ImageDraw.Draw(sp)
for _ in range(260):
    x=random.gauss(W*0.5,900); y=random.gauss(H*0.6,380); r=random.choice([2,3,4,6])
    ds.ellipse([x-r,y-r,x+r,y+r],fill=(255,210,150,random.randint(90,230)))
base.alpha_composite(sp.filter(ImageFilter.GaussianBlur(1.5)))
# vignette
v=np.clip(1.15-((xx/W-0.5)**2*1.2+(yy/H-0.55)**2*1.6)*1.6,0.35,1)
arr=np.asarray(base.convert('RGB'),np.float32)*v[:,:,None]
bg=Image.fromarray(np.clip(arr,0,255).astype(np.uint8))
bg.save('./background-source.png')

def title_layer(size, scale):
    L=Image.new('RGBA',size,(0,0,0,0))
    big=ImageFont.truetype('/usr/share/fonts/truetype/google-fonts/Poppins-Bold.ttf',int(330*scale))
    small=ImageFont.truetype('/usr/share/fonts/truetype/google-fonts/Poppins-Bold.ttf',int(96*scale))
    tag=ImageFont.truetype('/usr/share/fonts/truetype/google-fonts/Poppins-Bold.ttf',int(70*scale))
    return L,big,small,tag

def draw_title(canvas,x,y,scale,with_tag=True):
    W2,H2=canvas.size
    L,big,small,tag=title_layer((W2,H2),scale)
    d=ImageDraw.Draw(L)
    t1="SUPER SMASH BROS."; t2="MELEE"
    d.text((x+8*scale,y),t1,font=small,fill=(235,240,255,255))
    by=y+int(95*scale)
    # gradient-filled MELEE
    mask=Image.new('L',(W2,H2),0); ImageDraw.Draw(mask).text((x,by),t2,font=big,fill=255)
    bbox=mask.getbbox()
    grad=np.zeros((H2,W2,4),np.uint8)
    yy=np.linspace(0,1,H2)[:,None]
    t=np.clip((yy*H2-bbox[1])/max(1,bbox[3]-bbox[1]),0,1)
    top=np.array([200,235,255]); bot=np.array([60,110,255])
    col=top*(1-t[...,None])+bot*t[...,None]
    grad[:,:,:3]=np.broadcast_to(col,(H2,1,3)).astype(np.uint8); grad[:,:,3]=255
    gimg=Image.fromarray(grad,'RGBA'); gimg.putalpha(mask)
    # outline + shadow
    outline=mask.filter(ImageFilter.MaxFilter(max(3,int(17*scale)|1)))
    sh=Image.new('RGBA',(W2,H2),(10,5,30,0)); sh.putalpha(outline.filter(ImageFilter.GaussianBlur(18*scale)).point(lambda v:int(v*0.8)))
    ol=Image.new('RGBA',(W2,H2),(255,255,255,255)); ol.putalpha(outline)
    canvas.alpha_composite(sh,(int(10*scale),int(14*scale)))
    canvas.alpha_composite(ol); canvas.alpha_composite(gimg); canvas.alpha_composite(L)
    if with_tag:
        tw=d.textlength("PS5 EDITION",font=tag)
        bx=bbox[0]+8*scale; byy=bbox[3]+int(40*scale)
        badge=Image.new('RGBA',(W2,H2),(0,0,0,0)); db=ImageDraw.Draw(badge)
        db.rounded_rectangle([bx,byy,bx+tw+int(60*scale),byy+int(100*scale)],radius=int(22*scale),fill=(255,150,70,235))
        db.text((bx+int(30*scale),byy+int(6*scale)),"PS5 EDITION",font=tag,fill=(30,12,40,255))
        canvas.alpha_composite(badge)

sel=bg.convert('RGBA'); draw_title(sel,260,330,1.25); sel.convert('RGB').save('./pic0.png')
launch=bg.convert('RGBA'); draw_title(launch,int(W/2-1200*0.95/2*1.0)-330,300,1.05); launch.convert('RGB').save('./pic1.png')
# icon 512: crop of the sun/arena with title
ic=bg.crop((int(W*0.66-1000),int(H*0.70-1300),int(W*0.66+1000),int(H*0.70+700))).resize((1024,1024),Image.LANCZOS).convert('RGBA')
draw_title(ic,90,150,0.62,with_tag=True)
ic=ic.resize((512,512),Image.LANCZOS).convert('RGB'); ic.save('./icon0.png')
print('ok')

"""Offline HDR light-show baker. No runtime dependency on Python or ffmpeg.
Produces seamless eight-second H.264 loops and matching PNG posters, on black
for additive compositing into the native login UI. All geometry is original.
"""
from pathlib import Path
import argparse, sys, subprocess, time, json
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'BuildLogs/LoginRenderDeps'))
import numpy as np
from scipy.ndimage import gaussian_filter, zoom
from PIL import Image
import imageio_ffmpeg
W,H=1280,800
POSTERS=ROOT/'Tools/Art/Login/Loops'
MOVIES=ROOT/'UnrealGCS/Content/Movies/Login'
POSTERS.mkdir(parents=True,exist_ok=True);MOVIES.mkdir(parents=True,exist_ok=True)
BLUE=np.array([.30,.57,1.0]);GOLD=np.array([1.0,.66,.26]);CYAN=np.array([.22,.88,1.0])
rng=np.random.default_rng(4214)
y=rng.uniform(-1,1,12500);a=rng.uniform(0,np.pi*2,len(y));r=np.sqrt(1-y*y)
sphere=np.c_[r*np.cos(a),y,r*np.sin(a)]
noise=rng.random(len(y));energy=rng.lognormal(0,.5,len(y))
starxy=rng.uniform([-1.95,-1.22],[1.95,1.22],(190,2));starbright=rng.uniform(.2,1,190)

def splat(canvas,xy,colors,power):
    # Bilinear subpixel deposit preserves smooth motion after HDR bloom and downsample.
    x=W*.5+xy[:,0]*285;y=H*.5+xy[:,1]*285
    ix=np.floor(x).astype(int);iy=np.floor(y).astype(int);fx=x-ix;fy=y-iy
    for dx,dy,weight in [(0,0,(1-fx)*(1-fy)),(1,0,fx*(1-fy)),(0,1,(1-fx)*fy),(1,1,fx*fy)]:
        good=(ix+dx>=0)&(ix+dx<W)&(iy+dy>=0)&(iy+dy<H)
        np.add.at(canvas,(iy[good]+dy,ix[good]+dx),colors[good]*(power[good]*weight[good])[:,None])

def color_mix(base,white):return base*(1-white[:,None])+white[:,None]
def tilt(xy,angle=.22):
    c,s=np.cos(angle),np.sin(angle)
    return xy@np.array([[c,s],[-s,c]])

def build(theme,phase):
    canvas=np.zeros((H,W,3),np.float32)
    # Shared restrained peripheral stars; no image backdrop.
    good=np.linalg.norm(starxy,axis=1)>1.12
    col=np.tile(BLUE,(good.sum(),1));col[::5]=GOLD
    p=(starbright[good]**3*13+.5)*(.8+.2*np.sin(phase+np.arange(good.sum())))
    splat(canvas,starxy[good],col,p)
    if theme=='Saturn':
        yaw=.18*np.sin(phase);c,s=np.cos(yaw),np.sin(yaw)
        xyz=sphere.copy();xyz[:,0]=sphere[:,0]*c+sphere[:,2]*s;xyz[:,2]=-sphere[:,0]*s+sphere[:,2]*c
        vis=xyz[:,2]>0;pos=xyz[vis];n=noise[vis];limb=(1-pos[:,2])**3
        col=color_mix(BLUE,np.clip(limb*.35+n*.25,0,.7));col[n>.94]=GOLD*.8+.2
        power=energy[vis]*(3+n*4+limb*12)*(.86+.14*np.sin(phase+n*19))
        power[n>.994]*=4
        splat(canvas,tilt(pos[:,:2]),col,power)
        # Narrow silhouette glints, naturally dense along the visible sphere contour.
        theta=np.linspace(0,2*np.pi,1800,endpoint=False)
        rad=1+np.sin(theta*73)*.0025+np.sin(theta*113)*.001
        xy=np.c_[np.cos(theta)*rad,np.sin(theta)*rad]
        splat(canvas,xy,np.tile(BLUE*.65+.35,(len(theta),1)),2.8+2*np.sin(theta*19)**2)
        for band,rad in enumerate([1.29,1.33,1.41,1.48,1.55,1.60,1.73]):
            theta=np.linspace(0,2*np.pi,2400,endpoint=False)
            xyz=np.c_[rad*np.cos(theta),rad*.26*np.sin(theta),rad*.966*np.sin(theta)]
            rr=np.sum(xyz[:,:2]**2,axis=1);vis=(rr>=1)|(xyz[:,2]>np.sqrt(np.maximum(0,1-rr)))
            # Dense fine light threads punctuated by brighter traveling beads.
            pattern=.5+.5*np.sin(theta*103+phase+band)
            p=.35+pattern**30*13
            p+=((np.arange(len(theta))+band*5)%89==0)*17
            splat(canvas,tilt(xyz[vis,:2]),np.tile(GOLD,(vis.sum(),1)),p[vis])
    elif theme=='Wing':
        # Tapered folded ribbons, not solid rectangular chevrons.
        for layer in range(3):
            for side in [-1,1]:
                t,v=np.meshgrid(np.linspace(0,1,190),np.linspace(-1,1,19))
                width=.20*np.sin(t*np.pi)**1.1
                x=(layer-1)*.25+side*(t*1.38+v*width*.75)
                y=.62-layer*.20-t*1.38+.19*np.sin(t*np.pi)+v*width
                y+=.035*np.sin(phase+t*2)*np.sin(t*np.pi)
                x+=.003*np.sin(t*319+v*91)
                xy=tilt(np.c_[x.ravel(),y.ravel()],-.07)
                wave=(.5+.5*np.cos(t.ravel()*4-phase+layer))**8
                edge=np.abs(v.ravel())**10
                col=np.tile(CYAN*.8+.2,(len(xy),1))
                if layer==1:col[::13]=GOLD
                irregular=.4+.6*(.5+.5*np.sin(t.ravel()*871+v.ravel()*113))
                power=(1.5+wave*9+edge*8)*irregular*np.sin(t.ravel()*np.pi)**.35
                splat(canvas,xy,col,power)
    else:
        # Tapered light ribbons; jitter and intensity variation break the flag-like grid.
        for layer,base in enumerate([BLUE,CYAN,GOLD]):
            u,v=np.meshgrid(np.linspace(-1.82,1.82,250),np.linspace(-1,1,19))
            taper=np.maximum(0,np.cos(u/1.83*np.pi*.5))**.7
            x=u+.004*np.sin(u*153+v*45)
            y=.31*np.sin(u*2.1-phase+layer*.55)+v*.17*taper+.23*(layer-1)
            y+=.09*np.sin(u*3.1+phase)*taper+.002*np.cos(u*381+v*53)
            xy=np.c_[x.ravel(),y.ravel()]
            envelope=np.clip((1.83-np.abs(u.ravel()))/.3,0,1)
            irregular=.35+.65*(.5+.5*np.sin(u.ravel()*743+v.ravel()*233))**2
            power=envelope*(2+(.5+.5*np.cos(u.ravel()*3-phase))**8*9+np.abs(v.ravel())**10*6)*irregular
            splat(canvas,xy,np.tile(base,(len(xy),1)),power)
    # HDR optical spread is baked offline; no per-light bloom or geometry in UE.
    core=gaussian_filter(canvas,(.48,.48,0),mode='constant')
    bloom=gaussian_filter(canvas,(2.2,2.2,0),mode='constant')*.7
    small=canvas.reshape(H//4,4,W//4,4,3).mean((1,3))
    broad=gaussian_filter(small,(3.6,3.6,0),mode='constant')*.35
    broad=zoom(broad,(4,4,1),order=1,prefilter=False)
    hdr=core+bloom+broad
    # Exponential filmic shoulder keeps a bright core while preserving colored glow.
    linear=1-np.exp(-hdr*.72)
    rgb=np.where(linear<=.0031308,linear*12.92,1.055*linear**(1/2.4)-.055)
    return (np.clip(rgb,0,1)*255+.5).astype(np.uint8)

def main():
    parser=argparse.ArgumentParser();parser.add_argument('--theme',choices=['Saturn','Wing','Tide','all'],default='Saturn');parser.add_argument('--poster',action='store_true');args=parser.parse_args()
    themes=['Saturn','Wing','Tide'] if args.theme=='all' else [args.theme]
    for theme in themes:
        start=time.time();frame=build(theme,0);Image.fromarray(frame).save(POSTERS/(theme+'.png'))
        if args.poster:continue
        fps=24;count=192
        cmd=[imageio_ffmpeg.get_ffmpeg_exe(),'-y','-f','rawvideo','-vcodec','rawvideo','-s',f'{W}x{H}','-pix_fmt','rgb24','-r',str(fps),'-i','-','-an','-c:v','libx264','-preset','medium','-crf','16','-pix_fmt','yuv420p','-profile:v','high','-level','4.0','-movflags','+faststart','-threads','4',str(MOVIES/(theme+'.mp4'))]
        with open(POSTERS/(theme+'-encode.log'),'w') as log:
            proc=subprocess.Popen(cmd,stdin=subprocess.PIPE,stderr=log)
            for i in range(count):
                proc.stdin.write((frame if i==0 else build(theme,i/count*2*np.pi)).tobytes())
                if i%48==0:print(theme,i,'/',count,round(time.time()-start,1),'s',flush=True)
            proc.stdin.close()
            if proc.wait()!=0:raise RuntimeError('Encoding failed: '+theme)
        print('Completed',theme,round(time.time()-start,1),'s',flush=True)
        # Endpoint equality follows periodic geometry. Exclude duplicate endpoint frame.
        assert np.array_equal(build(theme,0),build(theme,2*np.pi))
        (POSTERS/(theme+'-manifest.json')).write_text(json.dumps(dict(theme=theme,width=W,height=H,fps=fps,frames=count,duration=8,seamless_endpoint=True),indent=2))
if __name__=='__main__':main()

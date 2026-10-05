import numpy as np, sys
from clipio import *
A,B,C=0.17883277,0.28466892,0.55991073
M709_TO_2020=np.array([[0.6274,0.3293,0.0433],[0.0691,0.9195,0.0114],[0.0164,0.0880,0.8956]])
M2020_TO_709=np.linalg.inv(M709_TO_2020)
def inv_oetf(e):
    return np.where(e<=0.5, e*e/3.0, (np.exp((e-C)/A)+B)/12.0)
def yuv_to_nits(y,u,v):
    yy=(y.astype(np.float64)-64)/876
    uu=np.repeat(np.repeat((u.astype(np.float64)-512)/896,2,0),2,1)
    vv=np.repeat(np.repeat((v.astype(np.float64)-512)/896,2,0),2,1)
    r=yy+1.4746*vv; b=yy+1.8814*uu; g=(yy-0.2627*r-0.0593*b)/0.6780
    e=np.stack([r,g,b],-1)
    s=inv_oetf(np.clip(e,0,None))
    ys=s@np.array([0.2627,0.6780,0.0593])
    d=1000.0*np.power(np.maximum(ys,1e-12),0.2)[...,None]*s
    return d@M2020_TO_709.T
crops={'speaker':(1700,360,576,640),'wall':(700,150,512,384),'slide':(2900,300,640,384),'drums':(2700,1150,512,384),'audience':(3100,1700,512,384),'piano':(200,1200,512,384)}
outs={k:open(f'../real/{k}.f32','wb') for k in crops}
for i,y,u,v in frames('seq120.yuv'):
    for k,(x,y0,w,h) in crops.items():
        rgb=yuv_to_nits(y[y0:y0+h,x:x+w],u[y0//2:(y0+h)//2,x//2:(x+w)//2],v[y0//2:(y0+h)//2,x//2:(x+w)//2])
        outs[k].write(rgb.astype(np.float32).tobytes())
for f in outs.values(): f.close()
import json; json.dump(crops,open('../real/crops.json','w'))
print('ok')

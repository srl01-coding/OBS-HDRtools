import numpy as np, cv2
from clipio import *
from hlg import *
B=40
ys=[];us=[];vs=[]
for i,y,u,v in frames('iframes.yuv', range(1,9)):
    ys.append(y.astype(np.float32)); us.append(u.astype(np.float32)); vs.append(v.astype(np.float32))
Y=np.array(ys); U=np.array(us); V=np.array(vs); n=len(Y)
M=Y.mean(0)
bm=Y.reshape(n,H//B,B,W//B,B).mean(axis=(2,4))
mot=np.abs(bm-np.median(bm,0)).max(0)
static=mot<1.0
Mb=cv2.GaussianBlur(M,(0,0),2)
tex=np.abs(M-Mb).reshape(H//B,B,W//B,B).mean(axis=(1,3))
for t in (0.5,0.75,1,1.5): print('tex<',t,(static&(tex<t)).mean())
flat=static&(tex<0.75)
print('static',static.mean(),'flat',flat.mean())
np.save('flat40.npy',flat); np.save('Imean.npy',M)
mask=np.repeat(np.repeat(flat,B,0),B,1)
def blockzero(x,Bs):
    h,w=x.shape
    m=x.reshape(h//Bs,Bs,w//Bs,Bs).mean(axis=(1,3),keepdims=True)
    return (x.reshape(h//Bs,Bs,w//Bs,Bs)-m).reshape(h,w)
D=[blockzero(Y[k]-Y[k+1],B) for k in range(n-1)]
lev=M[mask]
d=np.stack([x[mask] for x in D])
bins=[64,90,120,160,200,250,300,350,400,450,500,550,600,650,700,750,800,860,940]
print('level code (mean) | nits | sigma_code | sigma_nits | rel% | sigma_F | px')
rows=[]
for lo,hi in zip(bins[:-1],bins[1:]):
    s=(lev>=lo)&(lev<hi)
    if s.sum()<3000: continue
    sc=d[:,s].std()/np.sqrt(2)
    c=lev[s].mean()
    ny=float(code_to_nits_grey(c)); dn=float(code_to_nits_grey(c+0.5)-code_to_nits_grey(c-0.5))
    sn=sc*dn; sF=sn/((ny+0.1)*np.log(2))
    rows.append((c,ny,sc,sn,100*sn/ny,sF,s.sum()))
    print('%4d-%4d (%5.1f) | %8.3f | %.3f | %.4f | %5.2f%% | %.4f | %d'%(lo,hi,c,ny,sc,sn,100*sn/ny,sF,s.sum()))
np.save('noise_rows.npy',np.array(rows))
mh=mask[::2,::2]
DU=[blockzero(U[k]-U[k+1],20) for k in range(n-1)]; DV=[blockzero(V[k]-V[k+1],20) for k in range(n-1)]
du=np.stack([x[mh] for x in DU]); dv=np.stack([x[mh] for x in DV])
print('chroma sigma code: Cb %.3f Cr %.3f | luma all flat %.3f'%(du.std()/np.sqrt(2),dv.std()/np.sqrt(2),d.std()/np.sqrt(2)))
def ac(x,mask,dy,dx):
    hh,ww=x.shape
    a=x[:hh-dy,:ww-dx]; b=x[dy:,dx:]; m=mask[:hh-dy,:ww-dx]&mask[dy:,dx:]
    a=a[m]; b=b[m]; a=a-a.mean(); b=b-b.mean()
    return (a*b).mean()/np.sqrt((a*a).mean()*(b*b).mean())
print('luma noise autocorr lag: horizontal / vertical / diagonal')
acs=[]
for lag in range(1,9):
    h=np.mean([ac(x,mask,0,lag) for x in D]); v=np.mean([ac(x,mask,lag,0) for x in D]); g=np.mean([ac(x,mask,lag,lag) for x in D])
    acs.append((lag,h,v,g)); print(' %d: %.3f / %.3f / %.3f'%(lag,h,v,g))
np.save('noise_ac.npy',np.array(acs))
print('chroma (Cb, half-res) autocorr lag 1..4 h/v:')
for lag in range(1,5):
    print(' %d: %.3f / %.3f'%(lag,np.mean([ac(x,mh,0,lag) for x in DU]),np.mean([ac(x,mh,lag,0) for x in DU])))
# luma-chroma noise correlation
print('corr(dY at even px, dCb):', np.corrcoef(np.concatenate([x[::2,::2][mh] for x in D]), du.ravel())[0,1])
# power spectrum (radially averaged) of noise in flat blocks: use 40x40 blocks
ps=np.zeros((B,B))
cnt=0
for x in D:
    for by,bx in zip(*np.nonzero(flat)):
        blk=x[by*B:(by+1)*B,bx*B:(bx+1)*B]
        ps+=np.abs(np.fft.fft2(blk*np.outer(np.hanning(B),np.hanning(B))))**2; cnt+=1
ps/=cnt
fy=np.fft.fftfreq(B)[:,None]; fx=np.fft.fftfreq(B)[None,:]; r=np.sqrt(fx**2+fy**2)
print('radial power (normalised to lowest band): ')
edges=[0,0.05,0.1,0.15,0.2,0.25,0.3,0.35,0.4,0.45,0.5,0.71]
p0=None
for lo,hi in zip(edges[:-1],edges[1:]):
    s=(r>=lo)&(r<hi)&(r>0)
    if s.any():
        val=ps[s].mean(); p0=p0 or val
        print('  f %.2f-%.2f cyc/px: %.3f'%(lo,hi,val/p0))

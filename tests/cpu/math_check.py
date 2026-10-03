# Verbatim copy of the executable CPU reference from the implementation brief,
# section 15 (OBS_HDR_Toolkit_Claude_Code_Brief.md v1.0, 3 Oct 2026).
"""Original numerical checks for the proposed specification, not OBS integration tests."""
import json, math
from pathlib import Path
import numpy as np

LUMA = np.array([0.212639005871510, 0.715168678767756, 0.072192315360734])
RGB_XYZ = np.array([[0.412390799265959,0.357584339383878,0.180480788401834], LUMA, [0.019330818715592,0.119194779794626,0.950532152249661]])
BRAD = np.array([[0.8951,0.2664,-0.1614],[-0.7502,1.7135,0.0367],[0.0389,-0.0685,1.0296]])

def cross(a,b): return a[0]*b[1]-a[1]*b[0]
def forward_bilinear(q,u,v):
 a,b,c,d = np.asarray(q,float)
 return (1-u)*(1-v)*a+u*(1-v)*b+u*v*c+(1-u)*v*d

def inverse_bilinear(q,p):
 a,b,c,d = np.asarray(q,float); e=b-a; f=d-a; g=a-b+c-d; h=p-a
 aa=cross(g,f); bb=cross(e,f)+cross(h,g); cc=cross(h,e)
 if abs(aa)<1e-12:
  roots=[] if abs(bb)<1e-12 else [-cc/bb]
 else:
  disc=bb*bb-4*aa*cc
  if disc < -1e-12: return None
  z=-0.5*(bb+math.copysign(math.sqrt(max(0,disc)),bb))
  roots=[-bb/(2*aa)] if abs(z)<1e-15 else [z/aa,cc/z]
 candidates=[]
 for v in roots:
  e2=e+g*v; den=np.dot(e2,e2)
  if den<1e-24: continue
  u=np.dot(h-f*v,e2)/den
  if -1e-8 <= u <= 1+1e-8 and -1e-8 <= v <= 1+1e-8:
   residual=np.linalg.norm(forward_bilinear(q,u,v)-p)
   candidates.append((residual,u,v))
 return min(candidates)[1:] if candidates else None

def homography(q):
 mat=[]; rhs=[]
 for (u,v),(x,y) in zip([(0,0),(1,0),(1,1),(0,1)],q):
  mat.append([u,v,1,0,0,0,-x*u,-x*v]); rhs.append(x)
  mat.append([0,0,0,u,v,1,-y*u,-y*v]); rhs.append(y)
 return np.r_[np.linalg.solve(mat,rhs),1.0].reshape(3,3)

def project(h,p):
 p=h@np.r_[p,1.0]; return p[:2]/p[2]

def toe(y,knee,strength):
 y=np.asarray(y,float)
 if knee<=0 or strength==0: return y.copy()
 t=np.clip(y/knee,0,1)
 scale=(1-strength)+strength*t*(2-t)
 return np.where((y>=0)&(y<knee),y*scale,y)

def shoulder(y,peak,softness):
 y=np.asarray(y,float)
 if softness==0: return np.minimum(y,peak)
 width=peak*softness; knee=peak-width; x=np.maximum(y-knee,0)
 return np.where(y>knee,knee+width*x/(width+x),y)

def daylight_xy(t):
 if not 4000<=t<=25000: raise ValueError('temperature outside daylight domain')
 if t<=7000: x=-4.6070e9/t**3+2.9678e6/t**2+0.09911e3/t+0.244063
 else: x=-2.0064e9/t**3+1.9018e6/t**2+0.24748e3/t+0.237040
 return np.array([x,-3*x*x+2.87*x-0.275])
def xy_uv(xy):
 x,y=xy; d=-2*x+12*y+3; return np.array([4*x/d,6*y/d])
def uv_xy(uv):
 u,v=uv; d=4+2*u-8*v; return np.array([3*u/d,2*v/d])
def white_xyz(xy):
 x,y=xy; return np.array([x/y,1,(1-x-y)/y])
def target_white(mired,tint):
 m0=1e6/6504
 def locus(m): return xy_uv(daylight_xy(1e6/m))
 uv0=xy_uv([0.3127,0.3290]); m=m0+mired
 uv=uv0+locus(m)-locus(m0)
 tangent=locus(m+0.1)-locus(m-0.1)
 normal=np.array([tangent[1],-tangent[0]]); normal/=np.linalg.norm(normal)
 if normal[1]>0: normal=-normal
 return white_xyz(uv_xy(uv+tint*0.01*normal))
def wb_matrix(mired,tint):
 if mired==0 and tint==0: return np.eye(3)
 w0=white_xyz([0.3127,0.3290]); wt=target_white(mired,tint)
 a=np.linalg.inv(BRAD)@np.diag((BRAD@wt)/(BRAD@w0))@BRAD
 return np.linalg.inv(RGB_XYZ)@a@RGB_XYZ

rng=np.random.default_rng(7)
quads=[np.array(x,float) for x in [
 [[0,0],[1,0],[1,1],[0,1]],
 [[.2,0],[.8,0],[1,1],[0,1]],
 [[-.3,.2],[1.1,-.1],[.9,1.2],[.1,.8]],
 [[0,0],[1,.2],[.9,1.2],[-.1,1]],
]]
max_bil=0.0; max_hom=0.0; n=0
for q in quads:
 h=homography(q); hi=np.linalg.inv(h)
 for uv in rng.random((1000,2)):
  p=forward_bilinear(q,*uv); inv=inverse_bilinear(q,p)
  assert inv is not None
  max_bil=max(max_bil,float(np.max(np.abs(np.array(inv)-uv))))
  max_hom=max(max_hom,float(np.max(np.abs(project(hi,project(h,uv))-uv))))
  n+=1
ys=np.r_[0,np.geomspace(1e-9,1e10,10000)]
for beta in [0,.25,.5,1]:
 out=toe(ys,.1,beta)
 assert np.all(np.diff(out)>=0) and np.all(out<=ys+1e-12) and out[0]==0
for soft in [0,.01,.25,.75]:
 out=shoulder(ys,1000,soft)
 assert np.all(np.diff(out)>=0) and np.all(out<=1000+1e-9) and out[0]==0
for m in [-90,-30,0,30,90]:
 for tint in [-1,0,1]:
  mat=wb_matrix(m,tint); white=mat@np.ones(3)
  assert np.isfinite(mat).all() and abs(LUMA@white-1)<1e-12
  xyz=RGB_XYZ@white
  assert np.allclose(xyz,target_white(m,tint),atol=1e-12)
assert np.array_equal(wb_matrix(0,0),np.eye(3))
# Relative-luminance color balance directions and saturation.
max_luma_err=0
for _ in range(2000):
 rgb=rng.uniform(-10,1000,3); y=LUMA@rgb; theta=rng.uniform(0,2*np.pi)
 d=np.cos(theta+np.array([0,-2*np.pi/3,2*np.pi/3])); d-=LUMA@d; d/=np.linalg.norm(d)
 balanced=rgb+max(y,0)*.5*d
 sat=y+rng.uniform(0,2)*(balanced-y)
 max_luma_err=max(max_luma_err,abs(LUMA@sat-y))
assert max_luma_err<1e-10
# Finite difference at soft shoulder and toe knee.
h=1e-5
slope_low=(toe(.1+h,.1,1)-toe(.1-h,.1,1))/(2*h)
slope_high=(shoulder(750+h,1000,.25)-shoulder(750-h,1000,.25))/(2*h)
assert abs(slope_low-1)<1e-3 and abs(slope_high-1)<1e-3
report={
 'scope':'CPU double-precision design equations only; no OBS build or GPU validation',
 'geometry_samples':n,'inverse_bilinear_max_abs_error':max_bil,
 'homography_roundtrip_max_abs_error':max_hom,
 'soft_clip_ramp_samples_per_parameter':len(ys),
 'white_balance_parameter_combinations':15,
 'luminance_preservation_random_samples':2000,
 'luminance_preservation_max_abs_error':max_luma_err,
 'toe_knee_numerical_derivative':float(slope_low),
 'shoulder_knee_numerical_derivative':float(slope_high),
 'sample_toe_0_0p025_0p05_0p075_0p1':toe([0,.025,.05,.075,.1],.1,1).tolist(),
 'sample_shoulder_0_750_1000_2000_10000':shoulder([0,750,1000,2000,10000],1000,.25).tolist(),
 'streamfx_style_center':forward_bilinear(quads[1],.5,.5).tolist(),
 'projective_center':project(homography(quads[1]),[.5,.5]).tolist(),
 'all_checks_passed':True,
}
Path(__file__).with_name('math_check_results.json').write_text(json.dumps(report,indent=2))
print(json.dumps(report,indent=2))


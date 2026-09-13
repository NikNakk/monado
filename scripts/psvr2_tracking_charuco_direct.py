#!/usr/bin/env python3
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: BSL-1.0
"""Direct native mode-4 ChArUco calibration for the four PSVR2 tracking cameras."""
from __future__ import annotations
import argparse,json,math
from pathlib import Path
import cv2,numpy as np
from scipy.optimize import least_squares
from scipy.spatial.transform import Rotation
W=H=508; MODE={0:(4,0),1:(4,1),2:(5,0),3:(5,1)}

def board_detector():
 d=cv2.aruco.getPredefinedDictionary(cv2.aruco.DICT_4X4_50)
 b=cv2.aruco.CharucoBoard((7,5),.04,.03,d)
 p=cv2.aruco.DetectorParameters();p.cornerRefinementMethod=cv2.aruco.CORNER_REFINE_SUBPIX;p.minMarkerPerimeterRate=.01;p.maxMarkerPerimeterRate=4
 c=cv2.aruco.CharucoParameters();c.minMarkers=2;c.tryRefineMarkers=True
 return b,cv2.aruco.CharucoDetector(b,c,p)

def dirs(root):
 out=sorted({p.parent for p in root.rglob('mode-04-size-*-set-4-example-*-plane0.pgm') if '__MACOSX' not in p.parts})
 if not out:raise ValueError('no mode-4 captures found')
 return out

def detect(d,cam,det):
 s,p=MODE[cam];ims=[]
 for f in sorted(d.glob(f'mode-04-size-*-set-{s}-example-*-plane{p}.pgm')):
  im=cv2.imread(str(f),0)
  if im is None or im.shape!=(508,512):raise ValueError(f'bad image {f}')
  ims.append(im[:,:W])
 m=np.mean(ims,0).astype(np.float32);best=(0,None,None,None)
 for hi in (4.,8.):
  im=np.clip(m*255/hi,0,255).astype(np.uint8);cc,ci,mc,mi=det.detectBoard(im);n=0 if ci is None else len(ci)
  if n>best[0]:best=(n,None if cc is None else np.asarray(cc,float).reshape(-1,2),None if ci is None else np.asarray(ci,int).reshape(-1),hi)
 return {'pose':d.name,'points':best[1],'ids':best[2],'corners':best[0],'stretch_hi':best[3]}

def KD(g):return np.array([[g[0],0,g[2]],[0,g[1],g[3]],[0,0,1.]],float),np.asarray(g[4:8],float).reshape(4,1)
def T(q):
 x=np.eye(4);x[:3,:3]=cv2.Rodrigues(np.ascontiguousarray(q[:3].reshape(3,1)))[0];x[:3,3]=q[3:];return x
def Q(x):return np.r_[cv2.Rodrigues(np.ascontiguousarray(x[:3,:3]))[0].ravel(),x[:3,3]]

def pnp(item,obj,g):
 K,D=KD(g);u=cv2.fisheye.undistortPoints(item['points'].reshape(-1,1,2),K,D).reshape(-1,2)
 ok,r,t=cv2.solvePnP(obj[item['ids']].reshape(-1,1,3),u.reshape(-1,1,2),np.eye(3),None,flags=cv2.SOLVEPNP_ITERATIVE)
 if not ok:raise RuntimeError(item['pose'])
 return np.r_[r.ravel(),t.ravel()]

def fit_intr(rows,obj,start=None,nfev=120):
 if start is None:start=np.array([190.,190.,254.,254.,.02,0,0,0])
 ps=[pnp(x,obj,start) for x in rows];x0=np.r_[start,*ps]
 lo=np.r_[80,80,-50,-50,[-1]*4,np.full(6*len(ps),-np.inf)];hi=np.r_[350,350,558,558,[1]*4,np.full(6*len(ps),np.inf)]
 def res(x):
  K,D=KD(x[:8]);pp=x[8:].reshape(-1,6);o=[]
  for row,q in zip(rows,pp):
   pr,_=cv2.fisheye.projectPoints(obj[row['ids']].reshape(-1,1,3),np.ascontiguousarray(q[:3,None]),np.ascontiguousarray(q[3:,None]),K,D)
   o.extend((pr.reshape(-1,2)-row['points']).ravel())
  o.extend((x[4:8]/3).tolist());return np.asarray(o)
 r=least_squares(res,x0,bounds=(lo,hi),loss='huber',f_scale=.8,max_nfev=nfev,xtol=1e-10,ftol=1e-10,gtol=1e-10)
 g=r.x[:8];pp=r.x[8:].reshape(-1,6);K,D=KD(g);errs=[];poses={};per=[]
 for row,q in zip(rows,pp):
  pr,_=cv2.fisheye.projectPoints(obj[row['ids']].reshape(-1,1,3),np.ascontiguousarray(q[:3,None]),np.ascontiguousarray(q[3:,None]),K,D);e=np.linalg.norm(pr.reshape(-1,2)-row['points'],axis=1);errs.extend(e);poses[row['pose']]=T(q);per.append({'pose':row['pose'],'corners':len(e),'rms_px':float(np.sqrt(np.mean(e*e)))})
 e=np.asarray(errs);return g,poses,{'rms_px':float(np.sqrt(np.mean(e*e))),'median_px':float(np.median(e)),'p95_px':float(np.percentile(e,95)),'per_pose':per}

def loo(rows,obj,g):
 errs=[];params=[];per=[]
 for i,held in enumerate(rows):
  h,_,_=fit_intr(rows[:i]+rows[i+1:],obj,g,60);params.append(h);q=pnp(held,obj,h);K,D=KD(h);pr,_=cv2.fisheye.projectPoints(obj[held['ids']].reshape(-1,1,3),np.ascontiguousarray(q[:3,None]),np.ascontiguousarray(q[3:,None]),K,D);e=np.linalg.norm(pr.reshape(-1,2)-held['points'],axis=1);errs.extend(e);per.append({'pose':held['pose'],'corners':len(e),'rms_px':float(np.sqrt(np.mean(e*e)))})
 e=np.asarray(errs);p=np.vstack(params);return {'rms_px':float(np.sqrt(np.mean(e*e))),'median_px':float(np.median(e)),'p95_px':float(np.percentile(e,95)),'per_pose':per,'parameter_stddev':p.std(0).tolist()}

def rdelta(a,b):return math.degrees(float(np.linalg.norm(Rotation.from_matrix(a[:3,:3].T@b[:3,:3]).as_rotvec())))

def init_rig(poses):
 C={0:np.eye(4)};diag={}
 for c in (1,2,3):
  vals=[]
  for n in sorted(set(poses[0])&set(poses[c])):vals.append((n,np.linalg.inv(poses[c][n]@np.linalg.inv(poses[0][n]))))
  scores=[sum(math.radians(rdelta(a,b))+10*np.linalg.norm(a[:3,3]-b[:3,3]) for _,b in vals) for _,a in vals];med=vals[int(np.argmin(scores))][1];good=[];samples=[]
  for n,x in vals:
   dt=1000*np.linalg.norm(x[:3,3]-med[:3,3]);dr=rdelta(med,x);ok=dt<30 and dr<8
   if ok:good.append(x)
   samples.append({'pose':n,'translation_mm':(1000*x[:3,3]).tolist(),'delta_mm':float(dt),'delta_deg':float(dr),'accepted':ok})
  R=Rotation.from_matrix(np.array([x[:3,:3] for x in good])).mean().as_matrix();t=np.median(np.array([x[:3,3] for x in good]),0);x=np.eye(4);x[:3,:3]=R;x[:3,3]=t;C[c]=x;diag[str(c)]={'samples':samples,'accepted':len(good)}
 return C,diag

def fit_rig(M,G,poses,obj):
 C,diag=init_rig(poses);names=sorted({x['pose'] for rows in M.values() for x in rows});B={}
 for n in names:
  cand=[C[c]@poses[c][n] for c in range(4) if n in poses[c]];x=np.eye(4);x[:3,:3]=Rotation.from_matrix(np.array([y[:3,:3] for y in cand])).mean().as_matrix();x[:3,3]=np.median(np.array([y[:3,3] for y in cand]),0);B[n]=x
 x0=np.r_[*[Q(C[c]) for c in (1,2,3)],*[Q(B[n]) for n in names]];flat=[(c,x) for c in range(4) for x in M[c]]
 def unpack(z):
  cc={0:np.eye(4)};o=0
  for c in (1,2,3):cc[c]=T(z[o:o+6]);o+=6
  bb={}
  for n in names:bb[n]=T(z[o:o+6]);o+=6
  return cc,bb
 def res(z):
  cc,bb=unpack(z);out=[]
  for c,row in flat:
   cb=np.linalg.inv(cc[c])@bb[row['pose']];r=cv2.Rodrigues(np.ascontiguousarray(cb[:3,:3]))[0];t=np.ascontiguousarray(cb[:3,3,None]);K,D=KD(G[c]);pr,_=cv2.fisheye.projectPoints(obj[row['ids']].reshape(-1,1,3),r,t,K,D);out.extend((pr.reshape(-1,2)-row['points']).ravel())
  return np.asarray(out)
 rr=least_squares(res,x0,loss='huber',f_scale=.8,max_nfev=100,xtol=1e-10,ftol=1e-10,gtol=1e-10);cc,bb=unpack(rr.x);e=res(rr.x).reshape(-1,2);d=np.linalg.norm(e,axis=1)
 return cc,{'rms_px':float(np.sqrt(np.mean(d*d))),'median_px':float(np.median(d)),'p95_px':float(np.percentile(d,95)),'initial_stability':diag}

def caljson(g):
 K,D=KD(g);return {'resolution':{'width':508,'height':508},'model':'fisheye_equidistant4','intrinsics':{'fx':float(K[0,0]),'fy':float(K[1,1]),'cx':float(K[0,2]),'cy':float(K[1,2])},'distortion':dict(zip(('k1','k2','k3','k4'),D.ravel().tolist()))}

def main():
 a=argparse.ArgumentParser();a.add_argument('capture_root',type=Path);a.add_argument('--output',required=True,type=Path);a.add_argument('--skip-cross-validation',action='store_true');z=a.parse_args();b,det=board_detector();obj=np.asarray(b.getChessboardCorners(),float);M={c:[] for c in range(4)}
 ds=dirs(z.capture_root);print('captures',len(ds))
 for d in ds:
  ns=[]
  for c in range(4):
   x=detect(d,c,det);ns.append(x['corners']);
   if x['corners']>=6:M[c].append(x)
  print(d.name,'/'.join(map(str,ns)))
 G={};P={};R={}
 for c in range(4):
  if len(M[c])<6:raise SystemExit(f'camera {c}: insufficient poses')
  G[c],P[c],fit=fit_intr(M[c],obj);cv=None if z.skip_cross_validation or c<2 else loo(M[c],obj,G[c]);R[str(c)]={'camera':c,'usable_poses':len(M[c]),'calibration':caljson(G[c]),'fit':fit,'leave_one_pose_out':cv};print('camera',c,'rms',fit['rms_px'],'params',G[c]);
  if cv:print('  LOO',cv['rms_px'],cv['median_px'],cv['p95_px'])
 C,rf=fit_rig(M,G,P,obj);print('rig',rf['rms_px'],rf['median_px'],rf['p95_px'])
 for c in range(4):print('T',c,C[c])
 out={'format':'psvr2-mode4-charuco-direct-calibration-v1','runtime_usable':False,'capture_root':str(z.capture_root),'board':{'squares':[7,5],'square_mm':40,'marker_mm':30,'dictionary':'DICT_4X4_50'},'cameras':R,'native_relative_rig':{'origin':'mode4_camera0_native_opencv','transforms_T_camera0_camera':{str(c):C[c].tolist() for c in range(4)},'fit':rf},'note':'Direct native mode-4 calibration. Absolute alignment of native camera0 to the headset/visible frame remains separate.'};z.output.write_text(json.dumps(out,indent=2)+'\n');print('wrote',z.output)
if __name__=='__main__':raise SystemExit(main())

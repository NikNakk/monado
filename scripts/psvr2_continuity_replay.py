#!/usr/bin/env python3
# SPDX-License-Identifier: BSL-1.0
"""Replay position continuity and accuracy together; requires NumPy.

Host-time correction decay is evaluated at the recorded query time, never at
future target time. Input/eligibility/truth are shared with predictor_replay.
"""
import argparse
import csv
import json
import subprocess
from pathlib import Path
import numpy as np
from psvr2_predictor_replay import load, read, states


def transition_states(t, host, p, v, valid, tau):
    cp, cv = np.zeros_like(p), np.zeros_like(v)
    for i in range(1, len(t)):
        dt = t[i]-t[i-1]
        dh = host[i]-host[i-1]
        if not (valid[i] and valid[i-1] and .005 <= dt <= .035 and 0 <= dh <= .1):
            continue
        decay = np.exp(-dh/tau)
        cp[i] = p[i-1]+v[i-1]*dt-p[i]+decay*(cp[i-1]+cv[i-1]*dt)
        cv[i] = v[i-1]-v[i]+decay*cv[i-1]
    return cp, cv


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--pids', nargs='+', required=True)
    parser.add_argument('--trace-dir', type=Path, default=Path('/tmp'))
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--c-predictor', type=Path)
    args=parser.parse_args();args.output.mkdir(parents=True, exist_ok=True)
    rows=[];metadata=[]
    for pid in args.pids:
        path=args.trace_dir/f'monado_psvr2_{pid}_slam.csv'
        data=load(path)
        if data is None:continue
        t,p,v,ix,h,actual,direction,raw,phases,masks,meta,valid,live=data
        s=read(path);tn=s['slam_vts_ns'];host=(s['host_received_ns']-s['host_received_ns'][0])*1e-9
        hp=path.with_name(f'monado_psvr2_{pid}_horizon.csv')
        q=read(hp if hp.exists() else path.with_name(f'monado_psvr2_{pid}_pose.csv'))
        source_key='source_slam_vts_ns' if hp.exists() else 'latest_slam_vts_ns'
        target_key='target_vts_ns' if hp.exists() else 'requested_vts_ns'
        host_key='query_host_ns' if hp.exists() else 'host_query_ns'
        # Same eligibility as load(), recovered by exact source/target pair.
        query_map={}
        for row in q:query_map.setdefault((int(row[source_key]),int(row[target_key])),[]).append(int(row[host_key]))
        seen={};query=[]
        for i,hh in zip(ix,h):
            key=(int(tn[i]),int(tn[i])+int(round(hh*1e9)));j=seen.get(key,0);query.append(query_map[key][j]);seen[key]=j+1
        query=(np.array(query,dtype=np.int64)-s['host_received_ns'][0])*1e-9
        age=query-host[ix]
        # host_query is recorded before lock acquisition, receipt before processing.
        # Exclude negative elapsed ages rather than silently admitting future data.
        eligible=age>=0
        meta['negative_host_age_excluded']=int((~eligible).sum());metadata.append(meta)
        baseline=np.linalg.norm(actual-raw,axis=1)*1000
        # At each source update, compare old/new functions at the first query's
        # target, at update host time and again 8.34 ms after it. The latter
        # measures correction delivered across a display period, not just C0.
        order=np.argsort(query,kind='stable');_,first=np.unique(ix[order],return_index=True);sample=order[first]
        sample=sample[(ix[sample]>=10)&eligible[sample]]
        ni=ix[sample];oi=ni-1;nh=h[sample];oh=nh+t[ni]-t[oi]
        jump_good=(t[ni]-t[oi]<.035)&(host[ni]>=host[oi])
        sample=sample[jump_good];ni=ni[jump_good];oi=oi[jump_good];nh=nh[jump_good];oh=oh[jump_good]
        candidates=[]
        for name,pp,vv,aa,kind in states(t,p,v,valid):
            if name not in ['raw','ema_0.25','ema_0.5','ema_0.75','bounded_acceleration'] and not name.startswith('abg_'):continue
            def predict(i,hh,hh_host,pp=pp,vv=vv,aa=aa,kind=kind):
                if kind=='bounded':
                    hc=np.minimum(hh,.08);return pp[i]+vv[i]*hh[:,None]+.5*aa[i]*(hc*(hc+t[i]-t[i-1]))[:,None]
                return pp[i]+vv[i]*hh[:,None]+.5*aa[i]*hh[:,None]**2
            candidates.append((name,predict))
        effective_a=next(aa for name,_,_,aa,_ in states(t,p,v,valid) if name=='bounded_acceleration')
        interval=np.r_[0,np.diff(t)]
        base_v=v+effective_a*interval[:,None]*.5
        for tau_ms in [2,4,8,12]:
            cp,cv,ca=np.zeros_like(p),np.zeros_like(v),np.zeros_like(v)
            for i in range(1,len(t)):
                dt=t[i]-t[i-1];dh=host[i]-host[i-1]
                if not(valid[i] and valid[i-1] and .005 <= dt <= .035 and 0 <= dh <= .1):continue
                decay=np.exp(-dh/(tau_ms*.001))
                cp[i]=p[i-1]+base_v[i-1]*dt+.5*effective_a[i-1]*dt*dt-p[i]+decay*(cp[i-1]+cv[i-1]*dt+.5*ca[i-1]*dt*dt)
                cv[i]=base_v[i-1]+effective_a[i-1]*dt-base_v[i]+decay*(cv[i-1]+ca[i-1]*dt)
                ca[i]=effective_a[i-1]-effective_a[i]+decay*ca[i-1]
            for limit_mm in [0,5]:
                def predict(i,hh,hh_host,cp=cp,cv=cv,ca=ca,tau=tau_ms*.001,limit=limit_mm*.001):
                    hc=np.minimum(hh,.08)
                    base=p[i]+v[i]*hh[:,None]+.5*effective_a[i]*(hc*(hc+interval[i]))[:,None]
                    correction=(cp[i]+cv[i]*hh[:,None]+.5*ca[i]*hh[:,None]**2)*np.exp(-np.maximum(0,hh_host-host[i])/tau)[:,None]
                    if limit:correction*=np.minimum(1,limit/np.maximum(np.linalg.norm(correction,axis=1),1e-12))[:,None]
                    return base+correction
                candidates.append((f'acc_decay_{tau_ms}ms_cap{limit_mm}',predict))
        for tau_ms in [2,4,8,12,16]:
            cp,cv=transition_states(t,host,p,v,valid,tau_ms*.001)
            for limit_mm in [0,5]:
                def predict(i,hh,hh_host,cp=cp,cv=cv,tau=tau_ms*.001,limit=limit_mm*.001):
                    correction=(cp[i]+cv[i]*hh[:,None])*np.exp(-np.maximum(0,hh_host-host[i])/tau)[:,None]
                    if limit:
                        correction*=np.minimum(1,limit/np.maximum(np.linalg.norm(correction,axis=1),1e-12))[:,None]
                    return p[i]+v[i]*hh[:,None]+correction
                candidates.append((f'decay_{tau_ms}ms_cap{limit_mm}',predict))
        for name,predict in candidates:
            predicted=predict(ix,h,query);error=np.linalg.norm(actual-predicted,axis=1)*1000
            if args.c_predictor and name=='acc_decay_4ms_cap5':
                order=np.argsort(ix,kind='stable');commands=[];qi=0
                for i in range(len(t)):
                    ns=int(round(t[i]*1e9));host_ns=int(round(host[i]*1e9))
                    values=' '.join(format(x,'.12g') for x in [*p[i],*v[i]])
                    commands.append(f'S {ns} {host_ns} {values} {int(valid[i])}')
                    while qi<len(order) and ix[order[qi]]==i:
                        j=order[qi]
                        commands.append(f'P {ns+int(round(h[j]*1e9))} {int(round(query[j]*1e9))}')
                        qi+=1
                result=subprocess.run([str(args.c_predictor),'--replay-continuity'],input='\n'.join(commands)+'\n',text=True,capture_output=True,check=True)
                cpred=np.array([[float(x) for x in line.split()] for line in result.stdout.splitlines()])
                delta=float(np.max(np.linalg.norm(cpred-predicted[order],axis=1))*1000)
                meta['continuity_c_parity_max_difference_mm']=delta
                assert delta<.002,(pid,delta)

            along=np.sum((actual-predicted)*direction,axis=1)*1000
            previous=predict(oi,oh,host[ni])
            jump=np.linalg.norm(predict(ni,nh,host[ni])-previous,axis=1)*1000
            correction8=np.linalg.norm(predict(ni,nh,host[ni]+.0083417)-previous,axis=1)*1000
            # Hypothetical old state also advances in host time: isolates the
            # effect of accepting this sample rather than ordinary filter decay.
            innovation8=np.linalg.norm(predict(ni,nh,host[ni]+.0083417)-predict(oi,oh,host[ni]+.0083417),axis=1)*1000
            for regime,mask in masks.items():
                m=mask&eligible;jm=mask[sample]
                if not m.any():continue
                pct=np.percentile(error[m],[50,95,99]);jp=np.percentile(jump[jm],[50,95,99]) if jm.any() else [float('nan')]*3
                cpct=np.percentile(correction8[jm],[50,95,99]) if jm.any() else [float('nan')]*3
                ipct=np.percentile(innovation8[jm],[50,95,99]) if jm.any() else [float('nan')]*3
                rows.append(dict(pid=pid,model=name,regime=regime,n=int(m.sum()),updates=int(jm.sum()),median_mm=pct[0],p95_mm=pct[1],p99_mm=pct[2],win_pct=float(np.mean(error[m]<baseline[m])*100),along_mean_mm=float(np.mean(along[m])),jump_median_mm=jp[0],jump_p95_mm=jp[1],jump_p99_mm=jp[2],correction8_median_mm=cpct[0],correction8_p95_mm=cpct[1],correction8_p99_mm=cpct[2],innovation8_p95_mm=ipct[1]))
        print(pid,len(ix),meta['negative_host_age_excluded'],flush=True)
    with (args.output/'summary.csv').open('w') as f:
        w=csv.DictWriter(f,fieldnames=list(rows[0]));w.writeheader();w.writerows(rows)
    (args.output/'manifest.json').write_text(json.dumps(metadata,indent=2)+'\n')


if __name__=='__main__':main()

#!/usr/bin/env python3
"""Sample system, GPU and Alyx memory every INTERVAL seconds into a CSV.

usage: memsample.py OUT.csv [--interval 2] [--process alyx_host]
Stops on Ctrl+C or when the watched process has come and gone.
"""
import argparse
import csv
import re
import subprocess
import time


def run(*cmd):
    return subprocess.run(cmd, capture_output=True, text=True).stdout


def swap_mb():
    m = re.search(r'used = ([\d.]+)M', run('sysctl', 'vm.swapusage'))
    return float(m.group(1)) if m else ''


def vm_stat():
    out = run('vm_stat')
    page = int(re.search(r'page size of (\d+)', out).group(1))
    vals = {}
    for key, name in [('Pages free', 'free'), ('Pages wired down', 'wired'),
                      ('Pages occupied by compressor', 'compressed'),
                      ('Pageouts', 'pageouts'), ('Swapouts', 'swapouts')]:
        m = re.search(re.escape(key) + r':\s+(\d+)', out)
        if m:
            n = int(m.group(1))
            vals[name] = n if name in ('pageouts', 'swapouts') else round(n * page / 2**20)
    return vals


def gpu_stats():
    out = run('ioreg', '-r', '-c', 'AGXAccelerator', '-d', '1')
    m = re.search(r'"PerformanceStatistics" = \{([^}]*)\}', out)
    vals = {}
    if m:
        for k, v in re.findall(r'"([^"]+)"=(\d+)', m.group(1)):
            vals[k] = int(v)
    return {
        'gpu_alloc_mb': round(vals.get('Alloc system memory', 0) / 2**20),
        'gpu_inuse_mb': round(vals.get('In use system memory', 0) / 2**20),
        'device_util': vals.get('Device Utilization %', ''),
        'renderer_util': vals.get('Renderer Utilization %', ''),
        'tiler_util': vals.get('Tiler Utilization %', ''),
        'split_scenes': vals.get('SplitSceneCount', ''),
        'pb_mb': round(vals.get('Allocated PB Size', 0) / 2**20),
    }


def footprint_mb(pid):
    out = run('footprint', '-p', str(pid))
    m = re.search(r'phys_footprint:\s+([\d.]+)\s*([KMG])B', out) or \
        re.search(r'Footprint:\s+([\d.]+)\s*([KMG])B', out)
    if not m:
        return ''
    scale = {'K': 1 / 1024, 'M': 1, 'G': 1024}[m.group(2)]
    return round(float(m.group(1)) * scale)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('out')
    ap.add_argument('--interval', type=float, default=2)
    ap.add_argument('--process', default='alyx_host')
    a = ap.parse_args()
    fields = ['t', 'swap_mb', 'free', 'wired', 'compressed', 'pageouts', 'swapouts',
              'gpu_alloc_mb', 'gpu_inuse_mb', 'device_util', 'renderer_util',
              'tiler_util', 'split_scenes', 'pb_mb', 'alyx_pid', 'alyx_footprint_mb']
    seen = False
    t0 = time.time()
    with open(a.out, 'w', newline='') as f:
        w = csv.DictWriter(f, fieldnames=fields)
        w.writeheader()
        try:
            while True:
                pids = run('pgrep', '-x', a.process).split()
                pid = pids[0] if pids else ''
                seen |= bool(pid)
                row = {'t': round(time.time() - t0, 1), 'swap_mb': swap_mb(),
                       'alyx_pid': pid,
                       'alyx_footprint_mb': footprint_mb(pid) if pid else ''}
                row.update(vm_stat())
                row.update(gpu_stats())
                w.writerow(row)
                f.flush()
                if seen and not pid:
                    break
                time.sleep(a.interval)
        except KeyboardInterrupt:
            pass


if __name__ == '__main__':
    main()

#!/usr/bin/env python3
"""Summarize AMUN F' runs from GDS event logs.
usage: python3 amun_results.py [logs_dir]   (default: newest session under AmunDeployment/logs)
"""
import os, re, sys, glob, statistics as st
from collections import defaultdict

base = sys.argv[1] if len(sys.argv) > 1 else max(
    glob.glob(os.path.expanduser('~/amun/fprime/amun-fsw/AmunDeployment/logs/*/')), key=os.path.getmtime)
files = sorted(glob.glob(os.path.join(base, '**', '*event.log'), recursive=True)) or [base]
lines = []
for f in files:
    if os.path.isfile(f):
        lines += open(f, errors='replace').read().splitlines()
print(f'{len(lines)} event lines from {files}')

trial = defaultdict(list); readout = []; other = defaultdict(int)
for ln in lines:
    m = re.search(r'Carry trial neuron (\d+): wNew=([\d.]+) T=(\d+)us t_x=(\d+)us ratio=([\d.]+)', ln)
    if m:
        trial[float(m.group(2))].append((int(m.group(1)), float(m.group(5)))); continue
    m = re.search(r'Readout neuron (\d+): state=(-?[\d.]+) charged=(-?[\d.]+) held=(-?[\d.]+)s drift=(-?[\d.]+)/s raw=(-?[\d.]+) corrected=(-?[\d.]+)', ln)
    if m:
        readout.append(tuple([int(m.group(1))] + [float(m.group(i)) for i in range(2, 8)])); continue
    m = re.search(r'Readout neuron (\d+): t_x=(\d+)us charged=(\d+)us retained ratio=([\d.]+)', ln)   # v4 format
    if m:
        readout.append((int(m.group(1)), int(m.group(2)) / 1000, int(m.group(3)) / 1000, float('nan'), float('nan'),
                        float(m.group(4)), float('nan'))); continue
    for key in ('GpsDegraded', 'GpsRecovered', 'GpsTrust', 'ModeChanged', 'Reattached', 'Crashing',
                'NoCross', 'ReadoutRail', 'HwError', 'Booted', 'Injected', 'LeakMeasured',
                'Recovered', 'Disagree', 'Repaired', 'Upset', 'CorruptedEst', 'TrainEpoch', 'InferNeuron', 'InferSummary',
                'Assigned', 'Baseline', 'Warn', 'Shed', 'Probe', 'Learned', 'Death', 'Escalated', 'WarmStart',
                'AuditFix', 'TaskEvent', 'OomKill'):
        if key in ln:
            other[key] += 1
            if key in ('GpsDegraded', 'GpsRecovered', 'GpsTrust', 'ModeChanged', 'Crashing', 'Reattached', 'HwError',
                       'ReadoutRail', 'LeakMeasured', 'Recovered', 'Disagree', 'Repaired', 'Upset', 'CorruptedEst',
                       'TrainEpoch', 'InferNeuron', 'InferSummary', 'Injected', 'Assigned', 'Baseline', 'Shed',
                       'Probe', 'Learned', 'Death', 'Escalated', 'WarmStart', 'AuditFix', 'TaskEvent', 'OomKill'):
                parts = ln.strip().split(',')
                print('  ', parts[0][11:23], ','.join(parts[5:])[:170])
            break

if trial:
    print('\nCARRY_TRIAL (ratio = t_x / T; expected 0.1/wNew; restore trials logged as wNew=0.1)')
    for w in sorted(trial):
        v = [r for _, r in trial[w]]
        print(f'  wNew={w:<5} expected {0.1 / w:.3f}  median {st.median(v):.3f}  sd {st.pstdev(v):.3f}  n={len(v)}')
        for k in (1, 2, 3, 4):
            vk = [r for n, r in trial[w] if n == k]
            if vk:
                print(f'      neuron {k}: median {st.median(vk):.3f} n={len(vk)}')
if readout:
    print('\nREADOUT (units = ms of +1 at w=0.1; corrected = (state + drift*held) / charged)')
    for n, stt, ch, held, dr, rr, rc in readout:
        print(f'  neuron {n}: state={stt:+8.3f} charged={ch:7.3f} held={held:7.1f}s drift={dr:+.3f}/s raw={rr:+.3f} corrected={rc:+.3f}')
# E8b: compare each Recovered value with the ground truth F' logged before the crash (evaluation only)
import datetime
truth = []
try:
    for t in open('/home/amun/f/est_truth.log'):
        v = t.split()
        if len(v) >= 5:
            truth.append((int(v[0]) / 1e9, [float(x) for x in v[1:5]]))
except OSError:
    pass
rec = []
for ln in lines:
    m = re.search(r'Recover neuron (\d+): src=(\S+) analog raw=(-?[\d.]+) corrected=(-?[\d.]+) checkpoint=(-?[\d.]+) '
                  r'age=(-?[\d.]+)s down=(-?[\d.]+)s applied=(-?[\d.]+) rail=(\S+)', ln)
    if m:
        t = datetime.datetime.fromisoformat(ln.split(',')[0]).timestamp()
        rec.append((t, int(m.group(1)), m.group(2), *[float(m.group(i)) for i in range(3, 9)], m.group(9)))
if rec and truth:
    print('\nE8b RECOVERY vs truth at the crash (units; truth = last estimate F\' logged before it died)')
    for t, n, src, raw, corr, ck, age, down, applied, rail in rec:
        tc = t - down
        before = [v for tt, v in truth if tt <= tc + 0.5]
        if not before:
            continue
        tr = before[-1][n - 1]
        print(f'  neuron {n} src={src:<10} truth={tr:+7.2f}  applied={applied:+7.2f} (err {applied - tr:+6.2f})  '
              f'analog corrected={corr:+7.2f} (err {corr - tr:+6.2f})  checkpoint={ck:+7.2f} (err {ck - tr:+6.2f}, age {age:.0f}s)  '
              f'down={down:.1f}s rail={rail}  decision {"OK" if (applied > 0) == (tr > 0) else "WRONG"}')
# v7 summary (E12 reset loop / E13 continuous learning / E14 analog clock)
deaths = [re.search(r'down (-?[\d.]+) s \(analog clock (-?[\d.]+) s\), cause (\S+) \(progress (-?[\d.]+)\), policy (\S+)', l) for l in lines if 'Amun.neuro.Death' in l]
deaths = [d for d in deaths if d]
if deaths:
    print('\nv7 DEATHS (down = heartbeat downtime, clock = analog drift clock)')
    errs = []
    for d in deaths:
        dn, ck = float(d.group(1)), float(d.group(2))
        if dn > 0 and ck > 0:
            errs.append(ck - dn)
        print(f'  policy {d.group(5):<5} cause {d.group(3):<8} progress {float(d.group(4)):.2f}  down {dn:5.2f} s  clock {ck:5.2f} s')
    if errs:
        print(f'  E14 analog clock error: mean {st.mean(errs):+.2f} s, max |err| {max(abs(e) for e in errs):.2f} s, n={len(errs)}')
esc = [l for l in lines if 'Amun.neuro.Escalated' in l]
sheds = [l for l in lines if 'Amun.neuro.Shed' in l]
probes = [l for l in lines if 'Amun.neuro.Probe' in l]
learned = [l for l in lines if 'Amun.neuro.Learned' in l]
if deaths or sheds or esc:
    just = sum('justified = True' in l for l in probes)
    print(f'  deaths {len(deaths)}  escalations {len(esc)}  sheds {len(sheds)} (justified {just}, false {len(probes) - just})  learning updates {len(learned)}')
print('\nevent counts:', dict(other))

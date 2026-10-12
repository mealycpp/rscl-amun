#!/usr/bin/env python3
"""Seeded random stress schedule for E13. F' is never told which behaviours are faults.
usage (venv active, in AmunDeployment):  python3 amun_stress.py SEED MINUTES
"""
import random, subprocess, sys, time
seed, minutes = int(sys.argv[1]), float(sys.argv[2])
rng = random.Random(seed)
end = time.time() + minutes * 60
log = open('/home/amun/f/stress.log', 'a')
def cmd(kind, arg):
    r = subprocess.run(['fprime-cli', 'command-send', 'Amun.neuro.TASK', '--arguments', kind, str(arg)],
                       capture_output=True, text=True)
    line = f"{time.time():.3f} {kind} {arg} rc={r.returncode}"
    print(line, flush=True); log.write(line + '\n'); log.flush()
while time.time() < end:
    time.sleep(rng.uniform(20, 35))
    r = rng.random()
    if r < 0.45:   cmd('BURST', rng.choice((600, 900, 1200)))          # benign mission work
    elif r < 0.70: cmd('RUNAWAY', 500)                                 # fault
    elif r < 0.85: cmd('CACHE', rng.choice((4096, 6144)))              # benign memory use
    else:          cmd('LEAK', 12288)                                   # fault
print('stress schedule done')

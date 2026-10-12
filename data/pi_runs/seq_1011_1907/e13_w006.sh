#!/usr/bin/env bash
# E13 continuous learning from F''s own behaviour: learning ON vs OFF on the SAME seeded stress schedule.
# Unattended, ~2 x (MIN + 1) minutes. Needs GDS running with -n, venv active, run from AmunDeployment.
# usage: bash amun_e13.sh [minutes_per_arm=12] [seed=7]
MIN=${1:-12}; SEED=${2:-7}; W0=${3:-0.1}
R="bash $HOME/amun/fprime/neuro_pkg/amun_runs.sh"
SUP=$HOME/amun/fprime/neuro_pkg/amun_supervisor.sh
STRESS=$HOME/amun/fprime/neuro_pkg/amun_stress.py
D=/home/amun/f/e13_$(date +%m%d_%H%M); mkdir -p $D
stop_all() { pkill -f amun_stress; pkill -f amun_supervisor; sleep 0.5; pkill -x AmunDeployment; sleep 1; }
ss -ltn | grep -q ':50000 ' || { echo "GDS is not listening on 50000: start  fprime-gds -n --gui-addr 0.0.0.0"; exit 1; }
$R fault none >/dev/null
for L in 1 0; do
  stop_all
  bash "$SUP" 3 100 > $D/sup_learn$L.log 2>&1 &
  for i in $(seq 1 30); do grep -q "Connected to" $D/sup_learn$L.log && break; sleep 0.5; done
  $R cfg policy=AMUN learn=$L w0=0.06 b0=-18 audit=3 >/dev/null
  $R auto >/dev/null
  sleep 35                                   # BOOT + 30 s quiet baseline
  echo "=== learn=$L  start $(date +%H:%M:%S)  ($MIN min, seed $SEED)"
  python3 "$STRESS" $SEED $MIN > $D/stress_learn$L.log 2>&1
  sleep 15
done
stop_all
/usr/bin/python3 - "$D" <<'PY'
import re, sys, glob
D = sys.argv[1]
def ev_t(l):
    m = re.search(r'\(2:(\d+),(\d+)\)', l); return int(m.group(1)) + int(m.group(2)) / 1e6 if m else None
for L in ('1', '0'):
    lines = open(f'{D}/sup_learn{L}.log', errors='replace').read().splitlines()
    st = [l.split()[0] for l in open(f'{D}/stress_learn{L}.log') if l[:1].isdigit()]
    t0 = float(st[0]) - 30 if st else None
    deaths = [l for l in lines if 'DEATH detected' in l]
    sheds = [l for l in lines if ' SHED ' in l]
    pj = [l for l in lines if 'shed justified = 1' in l]; pf = [l for l in lines if 'shed justified = 0' in l]
    oom = [l for l in lines if 'OomKill' in l]; learned = [l for l in lines if 'LEARN ' in l]
    print(f'\n=== learning {"ON" if L == "1" else "OFF"}: deaths {len(deaths)} (OOM {len(oom)}), sheds {len(sheds)} '
          f'(justified {len(pj)}, false {len(pf)}), learning updates {len(learned)}, stress events {len(st)}')
    for l in learned: print('   ', l.split('(Amun.neuro) ')[-1])
    if t0:
        span = max(1.0, (float(st[-1]) - t0) / 3)
        for w in range(3):
            a, b = t0 + w * span, t0 + (w + 1) * span
            dw = sum(1 for l in deaths if ev_t(l) and a <= ev_t(l) < b)
            fw = sum(1 for l in pf if ev_t(l) and a <= ev_t(l) < b)
            jw = sum(1 for l in pj if ev_t(l) and a <= ev_t(l) < b)
            print(f'    window {w + 1}/3: deaths {dw}, false sheds {fw}, justified sheds {jw}')
PY
echo "E13 logs in $D"

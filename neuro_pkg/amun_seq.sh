#!/usr/bin/env bash
# AMUN unattended sequence (~2 h). Every arm is reported, whatever it shows (fixed before running).
#  A) reset loop, persisted timing fault started at a random phase: AMUN x10 vs B2 (5 s checkpoint) x10, interleaved
#  B) reset loop, persisted memory leak (20 MB per tick): B0 x1 as control, then AMUN x5
#  C) learning from F' behaviour, TIMING/MEMORY gains mis-tuned to w0=0.06, new seeds 11 and 23, learning ON vs OFF
# Needs GDS running WITHOUT the app on port 50000:  fprime-gds -n --gui-addr 0.0.0.0
# Run:  nohup bash ~/amun/fprime/neuro_pkg/amun_seq.sh > ~/seq_console.log 2>&1 &
set -u
cd "$HOME/amun/fprime/amun-fsw/AmunDeployment" || exit 1
. ../fprime-venv/bin/activate
PKG=$HOME/amun/fprime/neuro_pkg
R="bash $PKG/amun_runs.sh"
SUP=$PKG/amun_supervisor.sh
D=/home/amun/f/seq_$(date +%m%d_%H%M); mkdir -p "$D"
OUT=$D/RESULTS.txt
log() { echo "$*" | tee -a "$OUT"; }
stop_all() { pkill -f amun_stress; pkill -f amun_supervisor; sleep 0.5; pkill -x AmunDeployment; sleep 1; }
ss -ltn | grep -q ':50000 ' || { echo "GDS is not listening on 50000: start  fprime-gds -n --gui-addr 0.0.0.0"; exit 1; }

log "AMUN sequence start $(date '+%F %T')   results dir $D"
log "A: RUNAWAY 1000 at random onset 33-38 s after AUTONOMY; AMUN x10, B2 ckpt=5 x10 interleaved; watchdog 3 s, cap 6 deaths"
log "B: LEAK 20000 KB/tick, same protocol; B0 x1 control, AMUN x5"
log "C: amun_e13.sh with w0=0.06, 12 min per arm, seeds 11 and 23"
log ""

# arm ID GROUP "CFG" KIND VALUE
arm() {
  local id=$1 grp=$2 cfg=$3 kind=$4 val=$5 dly
  stop_all
  $R fault none >/dev/null
  bash "$SUP" 3 6 > "$D/sup_$id.log" 2>&1 &
  for i in $(seq 1 40); do grep -q "Connected to" "$D/sup_$id.log" && break; sleep 0.5; done
  $R cfg $cfg learn=0 w0=0.1 b0=-18 audit=3 >/dev/null
  $R auto >/dev/null                                   # BOOT, 10 s, AUTONOMY; 30 s quiet baseline follows
  dly=$(awk -v r=$RANDOM 'BEGIN{printf "%.2f", 33 + 5*r/32768}')
  sleep "$dly"
  $R fault "$kind" "$val" >/dev/null                   # persisted: every restart reloads it
  $R task "$kind" "$val" >/dev/null                    # and it starts now in the running instance
  sleep 100
  python3 - "$D/sup_$id.log" "$id" "$grp" "$cfg" "$kind $val" "$dly" >> "$OUT" <<'PY'
import sys, re
f, i, grp, cfg, fault, dly = sys.argv[1:7]
L = open(f, errors='replace').read().splitlines()
exits = sum(' exit pid=' in l for l in L)
wd = sum('WATCHDOG kill' in l for l in L)
oom = sum('Memory limit reached' in l for l in L)
dth = [re.search(r'cause (\S+) \(progress ([-\d.]+)\)', l) for l in L if 'DEATH detected' in l]
esc = [re.search(r'ESCALATE to SAFE: cause (\S+),', l) for l in L if 'ESCALATE to SAFE' in l]
cap = any('supervisor stop' in l for l in L)
hw = sum('HwError' in l for l in L)
named = ','.join(f'{m.group(1)}:{float(m.group(2)):.2f}' for m in dth if m) or '-'
safe = ','.join(m.group(1) for m in esc if m) or '-'
print(f'{i} grp={grp} cfg="{cfg}" fault="{fault}" onset={dly}s deaths={exits}{" CAPPED" if cap else ""} '
      f'watchdog={wd} oom={oom} named=[{named}] SAFE={safe} hwerr={hw}')
PY
  tail -1 "$OUT"
}

# A: interleaved so drift in time affects both policies alike
for n in $(seq -w 1 10); do
  arm A$n-a A-AMUN "policy=AMUN" RUNAWAY 1000
  arm A$n-b A-B2ckpt5 "policy=B2 ckpt=5" RUNAWAY 1000
done
# B: memory-leak reset loop
arm B00 B-B0 "policy=B0" LEAK 20000
for n in 1 2 3 4 5; do arm B0$n B-AMUN "policy=AMUN" LEAK 20000; done
stop_all; $R fault none >/dev/null

log ""
log "=== Summary A/B"
python3 - "$OUT" >> "$OUT" <<'PY'
import sys, re, collections
g = collections.OrderedDict()
for l in open(sys.argv[1]):
    m = re.search(r'grp=(\S+) .* deaths=(\d+)( CAPPED)? .* named=\[([^\]]*)\] SAFE=(\S+)', l)
    if m: g.setdefault(m.group(1), []).append((int(m.group(2)), bool(m.group(3)), m.group(4), m.group(5)))
for k, v in g.items():
    d = [x[0] for x in v]
    one = sum(1 for x in v if x[0] == 1)
    safe = collections.Counter(x[3] for x in v)
    first = collections.Counter(x[2].split(',')[0].split(':')[0] for x in v)
    print(f'{k}: runs {len(v)}, deaths per run {d}, one-death runs {one}/{len(v)}, capped {sum(x[1] for x in v)}, '
          f'cause named at first death {dict(first)}, SAFE {dict(safe)}')
PY
tail -4 "$OUT"

# C: learning with mis-tuned gains on two new seeds (reuses amun_e13.sh unchanged except w0)
sed 's/w0=0\.1 b0/w0=0.06 b0/' "$PKG/amun_e13.sh" > "$D/e13_w006.sh"
if grep -q 'w0=0.06 b0' "$D/e13_w006.sh"; then
  for s in 11 23; do
    log ""; log "=== C seed $s (w0=0.06)"
    bash "$D/e13_w006.sh" 12 $s 2>&1 | tee -a "$OUT"
  done
else
  log "C skipped: could not set w0=0.06 in amun_e13.sh"
fi
stop_all; $R fault none >/dev/null
log ""; log "AMUN sequence done $(date '+%F %T')"

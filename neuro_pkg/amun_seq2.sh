#!/usr/bin/env bash
# AMUN unattended sequence 2 (~95 min). Every arm is reported, whatever it shows (fixed before running).
#  D) reset loop, persisted timing fault at a random phase: B0, B1 (K=3) and B2 (1 s checkpoint), 5 runs each, interleaved
#  E) learning from F' behavior with TIMING/MEMORY gains mis-tuned HIGH (w0=0.2), so benign bursts can cause false sheds;
#     learning ON vs OFF, seeds 7 and 11 (12 min per arm). Tests LEARN with delta=-1.
# Needs GDS running WITHOUT the app on port 50000:  fprime-gds -n --gui-addr 0.0.0.0
# Run:  nohup bash ~/amun/fprime/neuro_pkg/amun_seq2.sh > ~/seq2_console.log 2>&1 &
set -u
cd "$HOME/amun/fprime/amun-fsw/AmunDeployment" || exit 1
. ../fprime-venv/bin/activate
PKG=$HOME/amun/fprime/neuro_pkg
R="bash $PKG/amun_runs.sh"
SUP=$PKG/amun_supervisor.sh
D=/home/amun/f/seq2_$(date +%m%d_%H%M); mkdir -p "$D"
OUT=$D/RESULTS.txt
log() { echo "$*" | tee -a "$OUT"; }
stop_all() { pkill -f amun_stress; pkill -f amun_supervisor; sleep 0.5; pkill -x AmunDeployment; sleep 1; }
ss -ltn | grep -q ':50000 ' || { echo "GDS is not listening on 50000: start  fprime-gds -n --gui-addr 0.0.0.0"; exit 1; }

# E needs the learning script with w0=0.2, made from the w0=0.06 copy that already ran (its cfg line is known to work)
SRC=/home/amun/f/seq_1011_1907/e13_w006.sh
E13=$D/e13_w020.sh
if [ -f "$SRC" ] && grep -q 'w0=0.06' "$SRC"; then sed 's/w0=0\.06/w0=0.2/' "$SRC" > "$E13"; fi
if ! grep -q 'w0=0.2' "$E13" 2>/dev/null; then echo "cannot build $E13 from $SRC (no w0=0.06 found); stopping before any run"; exit 1; fi

log "AMUN sequence 2 start $(date '+%F %T')   results dir $D"
log "D: RUNAWAY 1000 at random onset 33-38 s after AUTONOMY; B0, B1 (K=3), B2 ckpt=1, 5 runs each interleaved; watchdog 3 s, cap 6 deaths"
log "E: $E13 (w0=0.2), 12 min per arm, learning ON then OFF, seeds 7 and 11"
log ""

# arm ID GROUP "CFG" KIND VALUE   (same protocol as amun_seq.sh)
arm() {
  local id=$1 grp=$2 cfg=$3 kind=$4 val=$5 dly
  stop_all
  $R fault none >/dev/null
  bash "$SUP" 3 6 > "$D/sup_$id.log" 2>&1 &
  for i in $(seq 1 40); do grep -q "Connected to" "$D/sup_$id.log" && break; sleep 0.5; done
  $R cfg $cfg learn=0 w0=0.1 b0=-18 audit=3 >/dev/null
  $R auto >/dev/null
  dly=$(awk -v r=$RANDOM 'BEGIN{printf "%.2f", 33 + 5*r/32768}')
  sleep "$dly"
  $R fault "$kind" "$val" >/dev/null
  $R task "$kind" "$val" >/dev/null
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

for n in 1 2 3 4 5; do
  arm D$n-b0 D-B0 "policy=B0" RUNAWAY 1000
  arm D$n-b1 D-B1 "policy=B1" RUNAWAY 1000
  arm D$n-c1 D-B2ckpt1 "policy=B2 ckpt=1" RUNAWAY 1000
done
stop_all; $R fault none >/dev/null

log ""
log "=== Summary D"
python3 - "$OUT" >> "$OUT" <<'PY'
import sys, re, collections
g = collections.OrderedDict()
for l in open(sys.argv[1]):
    m = re.search(r'grp=(\S+) .* deaths=(\d+)( CAPPED)? .* named=\[([^\]]*)\] SAFE=(\S+)', l)
    if m: g.setdefault(m.group(1), []).append((int(m.group(2)), bool(m.group(3)), m.group(4), m.group(5)))
for k, v in g.items():
    d = [x[0] for x in v]
    safe = collections.Counter(x[3] for x in v)
    first = collections.Counter(x[2].split(',')[0].split(':')[0] for x in v)
    print(f'{k}: runs {len(v)}, deaths per run {d}, capped {sum(x[1] for x in v)}, '
          f'cause at first death {dict(first)}, SAFE {dict(safe)}')
PY
tail -4 "$OUT"

for s in 7 11; do
  log ""; log "=== E seed $s (w0=0.2)"
  bash "$E13" 12 $s 2>&1 | tee -a "$OUT"
done
stop_all; $R fault none >/dev/null
log ""; log "AMUN sequence 2 done $(date '+%F %T')"

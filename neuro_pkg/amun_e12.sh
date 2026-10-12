#!/usr/bin/env bash
# E12 reset-loop experiment, all arms back to back, unattended (~11 min).
# Needs: GDS running WITHOUT the app (fprime-gds -n ...), venv active, run from AmunDeployment.
R="bash $HOME/amun/fprime/neuro_pkg/amun_runs.sh"
SUP=$HOME/amun/fprime/neuro_pkg/amun_supervisor.sh
OUT=/home/amun/f/e12_$(date +%m%d_%H%M).log
stop_all() { pkill -f amun_supervisor; sleep 0.5; pkill -x AmunDeployment; sleep 1; }
ss -ltn | grep -q ':50000 ' || { echo "GDS is not listening on 50000: start  fprime-gds -n --gui-addr 0.0.0.0"; exit 1; }
$R fault RUNAWAY 1000 >/dev/null
for arm in "policy=AMUN" "policy=B0" "policy=B1" "policy=B2 ckpt=1" "policy=B2 ckpt=5"; do
  stop_all
  bash "$SUP" 3 6 > /home/amun/f/sup_arm.log 2>&1 &
  for i in $(seq 1 30); do grep -q "Connected to" /home/amun/f/sup_arm.log && break; sleep 0.5; done
  $R cfg $arm learn=0 w0=0.1 b0=-18 audit=3 >/dev/null
  $R auto >/dev/null
  sleep 120
  echo "=== $arm" | tee -a "$OUT"
  grep -aE "DEATH detected|ESCALATE|WATCHDOG|supervisor stop|HwError" /home/amun/f/sup_arm.log | sed 's/.*(Amun.neuro) //' | tee -a "$OUT"
done
stop_all
$R fault none >/dev/null
echo "E12 done: $OUT"

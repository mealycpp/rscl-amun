#!/usr/bin/env bash
# AMUN board power, three states, logged by the FNIRSI meter at 100 samples/s (~9 min). Every state is reported.
# Wiring: PSU USB-C -> meter Type-C IN -> meter Type-C OUT -> Pi USB-C power.  Meter micro-USB (PC port) -> a Pi USB port.
# Run right after a cold power-on, so the FPAAs start unconfigured:
#   bash ~/amun/fprime/neuro_pkg/amun_power.sh
#  S1  Pi + HAT, chips unconfigured (cold boot), GDS running, F' not running
#  S2  chips configured, F' running AMUN (autonomy, quiet baseline, no fault, learning off)
#  S3  chips configured and holding, F' stopped
# S3 - S1 = power of the four configured FPAAs.  S2 - S3 = F' running AMUN (with GDS) on top.
set -u
cd "$HOME/amun/fprime/amun-fsw/AmunDeployment" || exit 1
. ../fprime-venv/bin/activate
PKG=$HOME/amun/fprime/neuro_pkg
R="bash $PKG/amun_runs.sh"
SUP=$PKG/amun_supervisor.sh
PM=$HOME/pm                                    # meter logger lives here
HOLD=${1:-120}                                 # seconds per state
D=/home/amun/f/power_$(date +%m%d_%H%M); mkdir -p "$D"
OUT=$D/steps.txt
mark() { echo "$(date '+%s.%3N %F %T') $*" | tee -a "$OUT"; }
stop_fsw() { pkill -f amun_supervisor; sleep 0.5; pkill -x AmunDeployment; sleep 1; }

[ -f "$PM/fnirsi_logger.py" ] || { echo "missing $PM/fnirsi_logger.py"; exit 1; }
lsusb | grep -qi '2e3c:0049' || { echo "meter not seen on USB (expected ID 2e3c:0049); check the micro-USB data cable"; lsusb; exit 1; }
pgrep -x AmunDeployment >/dev/null && { echo "F' is running; this needs a cold boot with F' off"; exit 1; }

echo "=== AMUN power, $(hostname), boot $(uptime -s), results dir $D" | tee -a "$OUT"
rm -f "$PM/fnirsi_stop"
( cd "$PM" && exec /usr/bin/python3 -u fnirsi_logger.py > "$D/meter.txt" 2> "$D/meter.err" ) &
sleep 5
[ "$(grep -c '^[0-9]' "$D/meter.txt")" -gt 100 ] || { echo "meter logger gives no samples:"; cat "$D/meter.err"; touch "$PM/fnirsi_stop"; exit 1; }
echo "meter logging: $(grep -c '^[0-9]' "$D/meter.txt") samples in 5 s" | tee -a "$OUT"

if ! ss -ltn | grep -q ':50000 '; then
  nohup fprime-gds -n --gui-addr 0.0.0.0 > "$D/gds.log" 2>&1 &
  for i in $(seq 1 60); do ss -ltn | grep -q ':50000 ' && break; sleep 1; done
fi
ss -ltn | grep -q ':50000 ' || { echo "GDS did not start"; touch "$PM/fnirsi_stop"; exit 1; }
sleep 20                                       # let GDS settle

mark "S1 start (chips unconfigured, F' off)"; sleep "$HOLD"; mark "S1 end"

$R fault none >/dev/null
bash "$SUP" 3 6 > "$D/sup.log" 2>&1 &
for i in $(seq 1 40); do grep -q "Connected to" "$D/sup.log" && break; sleep 0.5; done
$R cfg policy=AMUN learn=0 w0=0.1 b0=-18 audit=3 >/dev/null
$R auto >/dev/null
sleep 45                                       # BOOT + quiet baseline
P=$(pgrep -x AmunDeployment)
c0=$(awk '{print $14+$15}' /proc/$P/stat)
mark "S2 start (chips configured, F' running AMUN)"; sleep "$HOLD"; mark "S2 end"
c1=$(awk '{print $14+$15}' /proc/$P/stat 2>/dev/null || echo NA)
echo "F' pid $P CPU during S2: $(awk -v a=$c0 -v b=$c1 -v h=$HOLD -v t=$(getconf CLK_TCK) 'BEGIN{ if (b=="NA") print "NA (pid changed)"; else printf "%.1f%% of one core", 100*(b-a)/t/h }')" | tee -a "$OUT"
echo "deaths during S2: $(grep -c ' exit pid=' "$D/sup.log")" | tee -a "$OUT"

stop_fsw
sleep 5
mark "S3 start (chips configured, F' off)"; sleep "$HOLD"; mark "S3 end"

touch "$PM/fnirsi_stop"; sleep 3
echo "throttled: $(vcgencmd get_throttled 2>/dev/null)   (0x0 = no under-voltage)" | tee -a "$OUT"
python3 "$PKG/amun_power_cut.py" "$D/meter.txt" "$OUT" | tee -a "$OUT"
echo "done: $OUT"

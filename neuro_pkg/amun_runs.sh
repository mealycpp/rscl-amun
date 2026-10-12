#!/usr/bin/env bash
# AMUN run sheet: every experiment driven by F' commands only (no Python on the hardware).
# Terminal 1:  cd ~/amun/fprime/amun-fsw/AmunDeployment && . ../fprime-venv/bin/activate && fprime-gds --gui-addr 0.0.0.0
# Terminal 2:  cd ~/amun/fprime/amun-fsw/AmunDeployment && . ../fprime-venv/bin/activate && bash ~/amun/fprime/neuro_pkg/amun_runs.sh <step>
# Results: events land in AmunDeployment/logs/<gds session>/event.log ; summarize with amun_results.py
set -u
C() { fprime-cli command-send "Amun.neuro.$1" ${2:+--arguments} ${@:2} >/dev/null && echo "sent $*"; }
IMG=/home/amun/f/4n_H.ahf

case "${1:-}" in
  boot)        # R1: boot image, calibrate, zero, bias all 4 neurons
    C BOOT $IMG 0 ;;
  e7)          # R2: E7 inside F': carry + new weight, 3 reps per weight (12 trials x 4 neurons)
    for rep in 1 2 3; do for w in 0.05 0.1 0.2 0.4; do C CARRY_TRIAL 20 1000 $w false; sleep 3; done; done ;;
  ctrl)        # R3: control: weights rewritten 0.1->0.4->0.1 during the hold vs plain hold (wNew=0.1)
    for rep in 1 2 3 4; do C CARRY_TRIAL 20 1000 0.4 true; sleep 3; C CARRY_TRIAL 20 1000 0.1 false; sleep 3; done ;;
  monitor)     # R4: GPS integrity monitor on (watch GpsDegraded / GpsRecovered / GpsTrust events)
    C MONITOR true ;;
  app_carry)   # R5a: fault on neuron 4 (position jump), mode switch mid-fault, evidence KEPT
    C RESET_ON_MODE false; C MODE CRUISE; sleep 20
    C INJECT 4 100 30; sleep 5; C MODE LANDING; sleep 40; C MODE CRUISE ;;
  app_reset)   # R5b: same, baseline: mode switch WIPES evidence
    C RESET_ON_MODE true; C MODE CRUISE; sleep 20
    C INJECT 4 100 30; sleep 5; C MODE LANDING; sleep 40; C RESET_ON_MODE false; C MODE CRUISE ;;
  e8_hold)     # R6a: E8 crash in HOLD. Charge known state, then kill F' (restart GDS, then run e8_after)
    C MONITOR false; C CHARGE 15; date +%s.%N > /home/amun/f/crash_time; C CRASH false ;;
  e8_run)      # R6b: E8 crash in the middle of a +1 input (expect saturation)
    C MONITOR false; C CHARGE 15; date +%s.%N > /home/amun/f/crash_time; C CRASH true ;;
  e8_after)    # after restarting fprime-gds: reattach WITHOUT reset (identical HOLD bytes rewritten), read the state
    C REATTACH $IMG true; python3 -c "import time;print('downtime: %.1f s' % (time.time()-float(open('/home/amun/f/crash_time').read())))"; C READOUT ;;
  e8_after_nw) # same, but ZERO configuration writes at reattach (separates leak from reattach disturbance)
    C REATTACH $IMG false; python3 -c "import time;print('downtime: %.1f s' % (time.time()-float(open('/home/amun/f/crash_time').read())))"; C READOUT ;;
  e8b)         # E8b: evidence ramping on all 4 (labeled injection +15%), checkpoint every ${2:-20}s, crash 15 s in
    C BOOT $IMG 0; sleep 8; C REPAIR NONE; C SCRUB_PERIOD 0; C CKPT_PERIOD ${2:-20}
    for k in 1 2 3 4; do C INJECT $k 15 600; done
    C MONITOR true; sleep 15; C CRASH false ;;
  e8b_rec)     # after restarting fprime-gds: reattach without reset, recover from ANALOG | CHECKPOINT | COLD
    C REATTACH $IMG true; C RECOVER ${2:-ANALOG}; sleep 10; C MONITOR false ;;
  e9_sw)       # E9a: FPAA repairs software. Corrupt neuron 4's estimate, then a real fault: measure detection delay
    C BOOT $IMG 0; sleep 8; C CKPT_PERIOD 0; C SCRUB_PERIOD 0; C REPAIR ${2:-SCRUB}; C MONITOR true; sleep 10
    C CORRUPT_EST 4 15; sleep 8; C INJECT 4 100 15; sleep 20; C MONITOR false ;;
  e9_hw)       # E9b: software repairs FPAA. Hold-breaking config upset on neuron 4 (false alarm); policy SCRUB | FULL_RESET | NONE
    C BOOT $IMG 0; sleep 8; C CKPT_PERIOD 0; C SCRUB_PERIOD 0; C REPAIR ${2:-SCRUB}; C MONITOR true; sleep 10
    C UPSET 4 1; sleep 20; C MONITOR false ;;
  train)       # E10: chip-in-the-loop training from w = 0.1 (3 epochs, eta 0.15), about 8 min
    C SET_WEIGHTS 0 0.1 0.1; C TRAIN /home/amun/f/train.csv 3 0.15 ;;
  infer)       # E11: unseen episodes, untrained (w = 0.1) then trained weights, about 5 min
    C INFER /home/amun/f/test.csv false; C INFER /home/amun/f/test.csv true ;;
  cfg)         # v7: write amun.cfg, e.g.  $R cfg policy=AMUN learn=0 audit=10   (fault.cfg: $R fault RUNAWAY 500 | $R fault none)
    shift; printf "%s\n" "$@" > /home/amun/f/amun.cfg; cat /home/amun/f/amun.cfg ;;
  fault)       # v7: persisted configuration item loaded at every start (reset-loop carrier)
    if [ "${2:-none}" = none ]; then rm -f /home/amun/f/fault.cfg; echo "no persisted fault"; else echo "$2 $3" > /home/amun/f/fault.cfg; cat /home/amun/f/fault.cfg; fi ;;
  auto)        # v7: BOOT then start autonomous fault protection (30 s quiet baseline first)
    C BOOT $IMG 0; sleep 10; C AUTONOMY true ;;
  task)        # v7: one workload behaviour now, e.g.  $R task RUNAWAY 500
    C TASK $2 $3 ;;
  e8_ref)      # R6c: reference, no crash: charge, wait the same downtime, read
    C MONITOR false; C CHARGE 15; sleep "${2:-30}"; C READOUT ;;
  *) echo "steps: boot e7 ctrl monitor app_carry app_reset e8_hold e8_run e8_after e8_after_nw e8_ref | e8b [ckptS] e8b_rec ANALOG|CHECKPOINT|COLD | e9_sw SCRUB|NONE | e9_hw SCRUB|FULL_RESET|NONE | train infer | cfg k=v.. | fault KIND V|none | auto | task KIND V [seconds]" ;;
esac

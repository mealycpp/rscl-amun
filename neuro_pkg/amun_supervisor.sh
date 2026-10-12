#!/usr/bin/env bash
# AMUN supervisor (outside F'): restart F' whenever it exits; SIGKILL it when its heartbeat is older than W s.
# Run GDS without the app first:  fprime-gds -n --gui-addr 0.0.0.0
# usage: bash amun_supervisor.sh [W_seconds=3] [max_deaths=6]
W=${1:-3}; MAX=${2:-6}
APP=$HOME/amun/fprime/amun-fsw/build-artifacts/Linux/AmunDeployment/bin/AmunDeployment
HB=/home/amun/f/neuro.hb; LOG=/home/amun/f/supervisor.log
deaths=0; rm -f /home/amun/f/warm.flag /home/amun/f/boots.log   # fresh supervisor = fresh experiment
echo "$(date +%s.%N) supervisor start W=${W}s max=$MAX" >> $LOG
while [ $deaths -lt $MAX ]; do
  start_ns=$(date +%s%N)
  ( cd "$(dirname "$APP")" && exec "$APP" -a 127.0.0.1 -p 50000 ) &
  pid=$!; killed=0
  echo "$(date +%s.%N) start pid=$pid" | tee -a $LOG
  while kill -0 $pid 2>/dev/null; do
    sleep 0.2
    [ -f $HB ] || continue
    hb=$(cat $HB 2>/dev/null); [ -n "$hb" ] || continue
    [ "$hb" -gt "$start_ns" ] 2>/dev/null || continue          # armed only once this instance is alive
    age_ms=$(( ($(date +%s%N) - hb) / 1000000 ))
    if [ $age_ms -gt $(( W * 1000 )) ]; then
      kill -9 $pid; echo "$(date +%s.%N) WATCHDOG kill pid=$pid heartbeat age ${age_ms} ms" | tee -a $LOG; wait $pid 2>/dev/null; killed=1; break
    fi
  done
  if [ $killed = 1 ]; then rc=137; else wait $pid 2>/dev/null; rc=$?; fi
  deaths=$((deaths + 1))
  echo "$(date +%s.%N) exit pid=$pid rc=$rc deaths=$deaths" | tee -a $LOG
  sleep 1
done
echo "$(date +%s.%N) supervisor stop (max deaths reached)" | tee -a $LOG

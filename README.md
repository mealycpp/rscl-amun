<p align="center">
<img src="docs/logo/rscl.png" alt="RSCL" width="180">
</p>

<h1 align="center">AMUN</h1>
<p align="center"><b>Analog Memory in Unbroken Neurons for Breaking Spacecraft Reset Loops</b><br>
Reconfigurable Space Computing Lab (RSCL), Cal Poly Pomona</p>

---

When flight software (FSW) crashes, a watchdog restarts it, and the fault-protection evidence it gathered dies with it. If the fault persists, the restarted FSW crashes again before it can learn why, and the spacecraft enters a reset loop. AMUN keeps this evidence outside the software, as charge in the switched-capacitor integrators of field-programmable analog arrays (FPAAs), where it survives a kill of the FSW process between input pulses. The restarted FSW reads the charge without a chip reset or a configuration image, names the cause and resets only that subsystem to defaults. AMUN needs no evidence file, and it learns from its crashes through the surviving evidence and from false alarms through a probe.

AMUN runs as two NASA F′ components on a Raspberry Pi 4 with an Okika Pi.Ka HAT that carries four Anadigm AN231E04 FPAAs, one neuron per chip.

## Measured results

All results come from one board at room temperature, with NASA F′ v4.4. Every death is a SIGKILL of the F′ process.

| Experiment | Result |
|---|---|
| Analog state after a SIGKILL of F′ | Crash runs fall on the no-crash references; over 15 non-railed comparisons, the mean difference is −0.06 units and the largest 0.786 units (5.2 % of the 15-unit charge) |
| Gain rewrite on resident charge | Median discharge-to-charge ratios 1.890, 0.953, 0.477 and 0.243 for w = 0.05, 0.1, 0.2 and 0.4 (ideal 0.1/w); after dividing each neuron's median by its w = 0.1 median, the largest deviation from 0.1/w is 3.1 %; the rewrite keeps the resident charge |
| Reset loop, persistent timing fault | AMUN dies once and names TIMING at the first restart in 11 of 11 runs (the first run plus 10 at a random fault phase; 95 % Wilson interval [0.72, 1] for the 10), with no evidence file |
| Same fault, other restart policies | RAM-only evidence never breaks the loop (6 of 6 runs, stopped after six deaths). A three-reset boot counter needs three deaths and resets every subsystem (6 of 6). A 1 s checkpoint file names the cause after one death in 6 of 6 runs with 3600 writes per fault-free hour; a 5 s checkpoint does so in 6 of 10 ([0.31, 0.83]) with 720 |
| Reset loop, persistent memory leak | AMUN names MEMORY at the first restart in 5 of 5 runs, so 16 of 16 reset-loop runs in all; with RAM-only evidence, F′ dies six times |
| Learning from deaths, TIMING and MEMORY gains mis-tuned to w = 0.06 | 8, 12 and 9 deaths without learning versus 3, 3 and 3 with it, on three preset 12-min schedules; each of the nine updates follows a death, uses evidence that survived it and names the right monitor |
| Learning from false alarms, gains mis-tuned to w = 0.2 | Without learning, every work burst of 900 ms or more causes a false shed (10 and 5 on two schedules). With learning, the first false shed lowers TIMING's gain to 0.17 and its floor to −20, and none of the 13 later bursts is shed; all four runs shed every runaway task and leak without a death |
| Synthetic labeled episodes (GPS-monitor build) | Six gain updates, none after epoch 2; on 48 held-out episodes (32 with faults), detection rises from 30 to 32 and false alarms fall from 12 to 0 |
| Configuration upset in an input capacitor | The scrub repairs it in 71 ms and keeps the evidence; a full chip reset takes 372 ms, loses the evidence and trusts a degraded GPS for 1 s |
| Power at the 5 V input | 2.39 W idle, 2.73 W with the four FPAAs configured (+0.34 W), 3.49 W with F′ running AMUN and sending telemetry; F′ takes 4.8 % of one core and sends 11 eight-byte frames per tick |
| Hold drift over 57 logged boots | n1 −0.21 to 0.10, n2 1.68 to 1.96, n3 0.87 to 1.06, n4 2.79 to 3.15 units/s; the ranges do not overlap, so the role order holds in every boot |

## Limits

- One board at room temperature.
- The watchdog, F′ and the SPI master share one processor. AMUN targets faults, not a compromised F′, which owns the SPI bus.
- Retention is drift-bounded: from a 15-unit charge, the neurons reach the rail after about 21 s (n4) to more than 116 s (n1). On this board, a processor reboot loses the charge; surviving a whole-computer reset needs an analog board that the reset spares.
- No experiment compares AMUN with a shared-memory accumulator, an event-triggered checkpoint or ferroelectric RAM. No recovery-latency or radiation-tolerance benefit is claimed.
- Each tick, every policy also writes a heartbeat for the watchdog and the downtime estimate, and a test log of the estimates that no policy reads.
- The 16 reset-loop runs test no kill inside a monitor pulse. A death no monitor sees, such as one during initialization, names no cause, so AMUN still needs a boot counter.
- Learning at default gains has one run.

## Repository layout

| Path | Contents |
|---|---|
| `neuro_pkg/` | The package installed on the Pi: component sources, installers, run sheet, supervisor, experiment, stress and power scripts |
| `deployed/Components/` | The AmunNeuro and AnalogCompute components exactly as built in the F′ project |
| `deployed/Top/` | The F′ topology files after installation |
| `data/` | Data files behind the measured results (see below) |
| `data/pi_runs/` | Raw run folders copied from the Pi (supervisor logs, sequence results, learning-run logs, power-meter log) |
| `data/gds_logs.tgz` | F′ ground-system event logs, archived (when present) |
| `analysis/` | Scripts that recompute the gain-rewrite statistics and draw the measurement figures |
| `docs/` | Lab logo and sponsor logos |

| Data file | Experiment |
|---|---|
| `data/e7_trials.txt` | Gain rewrite on resident charge |
| `data/reboot_test_2026-10-11.txt` | Processor reboot test |
| `data/seq_1011_1907_RESULTS.txt` | Reset loop: AMUN and 5 s checkpoint at a random fault phase, memory leak |
| `data/seq_1011_1907_RESULTS_C.txt` | Learning from deaths, gains mis-tuned to w = 0.06, seeds 11 and 23 |
| `data/seq2_1018_1216_RESULTS_D.txt` | Reset loop: RAM-only, boot counter and 1 s checkpoint at a random fault phase |
| `data/seq2_1018_1216_RESULTS_E.txt` | Learning from false alarms, gains mis-tuned high |
| `data/power_1018_1433.txt` | Board power in three states |
| `data/power_1018_p1s_and_frames.txt` | Power at 1 s resolution with F′ running, and SPI frames per tick |
| `data/drift_boots_2026-10-18.txt` | Hold drift over 57 boots |

## Hardware and software

- Raspberry Pi 4 Model B, Raspberry Pi OS
- Okika Pi.Ka HAT with four AN231E04 FPAAs, driven over SPI, comparators read on GPIO
- USB GPS receiver (GPS-monitor build)
- NASA F′ v4.4, project at `~/amun/fprime/amun-fsw`, deployment `AmunDeployment`
- FNIRSI FNB48P USB meter for the power measurement

## Running

Install the components into the F′ project and build:

```bash
cd ~/amun/fprime/neuro_pkg
python3 install_neuro.py  ~/amun/fprime/amun-fsw
python3 install_analog.py ~/amun/fprime/amun-fsw
cd ~/amun/fprime/amun-fsw/AmunDeployment && . ../fprime-venv/bin/activate && fprime-util build
```

Single experiments go through the run sheet, with the ground system running in a second terminal:

```bash
# terminal 1
cd ~/amun/fprime/amun-fsw/AmunDeployment && . ../fprime-venv/bin/activate && fprime-gds --gui-addr 0.0.0.0
# terminal 2
cd ~/amun/fprime/amun-fsw/AmunDeployment && . ../fprime-venv/bin/activate
bash ~/amun/fprime/neuro_pkg/amun_runs.sh boot     # then e7, ctrl, e8_hold / e8_after, e9_hw, train, infer, ...
```

The reset-loop and learning experiments run unattended under the supervisor, which restarts F′ and kills it with SIGKILL when its heartbeat is older than 3 s. Start the ground system without the application first (`fprime-gds -n --gui-addr 0.0.0.0`), then:

```bash
bash ~/amun/fprime/neuro_pkg/amun_e12.sh            # reset loop, all policies
bash ~/amun/fprime/neuro_pkg/amun_e13.sh 12 7       # learning ON vs OFF, 12 min per arm, seed 7
nohup bash ~/amun/fprime/neuro_pkg/amun_seq.sh  > ~/seq_console.log 2>&1 &   # repeated reset-loop and learning runs
nohup bash ~/amun/fprime/neuro_pkg/amun_seq2.sh > ~/seq2_console.log 2>&1 &  # baselines at a random fault phase, false-alarm learning
```

The power measurement runs after a cold boot, with the meter in line at the Pi's USB-C input:

```bash
bash ~/amun/fprime/neuro_pkg/amun_power.sh
```

After F′ stops, the ground system's comm adapter can keep one core at 100 % and ignore SIGTERM; restart the ground system after stopping F′.

Recompute the gain-rewrite statistics and redraw the measurement figures (written to `fig/`):

```bash
python3 analysis/e7_stats.py
python3 analysis/make_figs.py
```

## Status

A paper describing AMUN is under review. A citation will be added here on publication.

## Author

Dr. Mohamed El-Hadedy, Department of Electrical and Computer Engineering, California State Polytechnic University, Pomona, and Coordinated Science Laboratory, University of Illinois Urbana-Champaign. Contact: mealy@cpp.edu

## RSCL sponsors and partners

<p align="center">
<img src="docs/sponsors/afrl.png" alt="afrl" height="60"> &nbsp;&nbsp; <img src="docs/sponsors/dod.png" alt="dod" height="60"> &nbsp;&nbsp; <img src="docs/sponsors/neec.png" alt="neec" height="60"> &nbsp;&nbsp; <img src="docs/sponsors/onr.png" alt="onr" height="60">
</p>

## License

No license is granted yet. Contact the author before reusing the code or data.

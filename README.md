<p align="center">
<img src="docs/logo/rscl.png" alt="RSCL" width="180">
</p>

<h1 align="center">AMUN</h1>
<p align="center"><b>Analog Memory in Unbroken Neurons for Flight-Software Recovery</b><br>
Reconfigurable Space Computing Lab (RSCL), Cal Poly Pomona</p>

---

Flight software (FSW) keeps its fault-protection evidence in process memory, so the evidence dies with the process. A watchdog restarts the FSW, and if the fault persists, the system enters a reset loop. AMUN stores this evidence as charge in the switched-capacitor integrators of field-programmable analog arrays (FPAAs). The charge survives a kill of the FSW process between input pulses. The restarted FSW reads it back without a chip reset or a configuration image, names the cause of its death and breaks the loop at the first restart, with no evidence written to nonvolatile storage.

AMUN runs as two NASA Fâ€² components on a Raspberry Pi 4 with an Okika Pi.Ka HAT that carries four Anadigm AN231E04 FPAAs, one neuron per chip.

## Measured results

| Experiment | Result |
|---|---|
| Analog state after a SIGKILL of Fâ€² | Matches no-crash references in 15 comparisons; largest difference 0.786 units, 5.2 % of the 15-unit charge |
| Gain rewrite on resident charge | Median discharge-to-charge ratios 1.890, 0.953, 0.477, 0.243 for w = 0.05, 0.1, 0.2, 0.4 (ideal 0.1/w); largest per-neuron deviation 3.1 % |
| Persistent timing fault, random onset | AMUN names TIMING at the first restart in 10 of 10 runs (95 % Wilson [0.72, 1]); a 5 s NV checkpoint does so in 6 of 10 ([0.31, 0.83]) with 720 evidence writes per hour |
| Persistent memory leak | AMUN names MEMORY at the first death in 5 of 5 runs; with RAM-only evidence, the supervisor stops Fâ€² after six deaths |
| Learning from deaths, gains mis-tuned to w = 0.06 | 8, 12 and 9 deaths without learning versus 3, 3 and 3 with it on three 12-min schedules; no false shed; 9 of 9 updates name the right class |
| Configuration upset in an input capacitor | Scrub repairs it in 71 ms and keeps the evidence; a full chip reset takes 372 ms and loses it |

Retention is drift-bounded (about 21 s to over 116 s depending on the neuron), and on this board a processor reboot resets the chips. AMUN targets faults, not a compromised Fâ€²: Fâ€² owns the SPI bus.

## Repository layout

| Path | Contents |
|---|---|
| `neuro_pkg/` | The package installed on the Pi: component sources, installers, run sheet, supervisor, experiment and stress scripts |
| `deployed/Components/` | The AmunNeuro and AnalogCompute components exactly as built in the Fâ€² project |
| `deployed/Top/` | The Fâ€² topology files after installation |
| `data/` | Data files behind the paper's numbers (E7 trials, reboot test, repeated reset-loop and learning runs) |
| `data/pi_runs/` | Raw run outputs copied from the Pi (supervisor logs, sequence results, learning-run logs) |
| `data/gds_logs.tgz` | Fâ€² ground-system event logs, archived (when present) |
| `analysis/` | Scripts that recompute the E7 statistics and draw the measurement figures |

## Hardware and software

- Raspberry Pi 4 Model B, Raspberry Pi OS
- Okika Pi.Ka HAT with four AN231E04 FPAAs, driven over SPI, comparators read on GPIO
- USB GPS receiver (GPS-monitor build)
- NASA Fâ€² v4.4, project at `~/amun/fprime/amun-fsw`, deployment `AmunDeployment`

## Running

Install the components into the Fâ€² project and build:

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

The reset-loop and learning experiments run unattended under the supervisor, which restarts Fâ€² and kills it with SIGKILL when its heartbeat is older than 3 s. Start the ground system without the application first (`fprime-gds -n --gui-addr 0.0.0.0`), then:

```bash
bash ~/amun/fprime/neuro_pkg/amun_e12.sh            # reset loop, all policies
bash ~/amun/fprime/neuro_pkg/amun_e13.sh 12 7       # learning ON vs OFF, 12 min per arm, seed 7
nohup bash ~/amun/fprime/neuro_pkg/amun_seq.sh > ~/seq_console.log 2>&1 &   # repeated runs, results in a text file
```

Recompute the E7 statistics from the logged trials:

```bash
python3 analysis/e7_stats.py
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

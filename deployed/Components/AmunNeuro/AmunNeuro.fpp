module Amun {

  @ Operating mode: selects the synaptic weight profile
  enum NeuroMode { CRUISE, LANDING }

  @ E8b: where a restarted F' takes each neuron's evidence from
  enum RecoverSrc { ANALOG, CHECKPOINT, COLD }

  @ E9: what F' does when its estimate and the analog comparator disagree
  enum RepairMode { NONE, SCRUB, FULL_RESET }

  @ v7: non-essential workload behaviours (the system never learns which ones are faults)
  enum TaskKind { RUNAWAY, BURST, LEAK, CACHE, GPS_BLOCK, CLEAR }

  @ Four analog evidence neurons on the PIKA HAT (one per AN231E04), driven entirely
  @ through the configuration interface: inputs (8-byte frames), weights (10-byte frames),
  @ comparator readout. Includes a GPS integrity monitor fed by a USB NMEA receiver.
  active component AmunNeuro {

    @ 1 Hz: read GPS, encode evidence, pulse neurons, read decisions
    async input port run: Svc.Sched drop

    # ---------------- commands ----------------
    @ Claim hardware, load the boot image (HOLD), calibrate polarity, zero and bias every neuron
    async command BOOT(
        path: string size 40 @< boot image (.ahf)
        spiHz: U32 @< 0 = 32 MHz
      )
    @ Set one neuron's input sample (+1 / 0 / -1) by rule (V1/V2 polarity bytes)
    async command SET_INPUT($neuron: U8, x: I8)
    @ Apply input x for a C++-timed duration, then return to 0
    async command PULSE($neuron: U8, x: I8, us: U32)
    @ Rewrite a neuron's synaptic weights by rule (capacitor ratios). neuron 0 = all
    async command SET_WEIGHTS($neuron: U8, wV1: F32, wV2: F32)
    @ E7 in flight software: charge (w=0.1) for T, hold, rewrite weights to wNew
    @ (restore=true: rewrite then restore 0.1 = control), return, time the crossing (all 4 neurons)
    async command CARRY_TRIAL(tMs: U16, holdMs: U32, wNew: F32, restore: bool)
    @ Enable / disable the 1 Hz GPS integrity monitor
    async command MONITOR(enable: bool)
    @ Change mode: rewrites the weight profile; resetOnMode=true wipes evidence (baseline)
    async command MODE(mode: NeuroMode)
    @ Baseline switch: re-boot the neurons on every mode change (evidence lost)
    async command RESET_ON_MODE(enable: bool)
    @ Test only: force a neuron's evidence (-100..100 %) for N seconds (labeled injection)
    async command INJECT($neuron: U8, pct: I8, seconds: U16)
    @ Release hardware handles (chain keeps running)
    async command CLOSE
    @ E8: zero every membrane, then charge all four with +1 for tMs (known state); checkpointed to file
    async command CHARGE(tMs: U16)
    @ E8: read every membrane in BOTH directions (above threshold: time -1 down; below: time +1 up);
    @ compares with the checkpoint and corrects for the boot-measured drift over the wall-clock hold time
    async command READOUT
    @ E8 TEST ONLY: kill this flight-software process now (inRun=true: in the middle of a +1 input)
    async command CRASH(inRun: bool)
    @ E8: re-open the hardware WITHOUT reset or reconfiguration; keep the analog state.
    @ writeHold=true: rewrite the (identical) HOLD input bytes; false: zero configuration writes
    async command REATTACH(path: string size 40, writeHold: bool)

    @ E8b: periodic software checkpoint of the evidence estimate during MONITOR (0 = off)
    async command CKPT_PERIOD(seconds: U16)
    @ E8b: after REATTACH, read every membrane, then resume MONITOR from the chosen source
    @ (ANALOG = membrane + drift x downtime, checkpoint fallback at the rail; CHECKPOINT; COLD = re-boot state)
    async command RECOVER(src: RecoverSrc)
    @ E9: repair policy on estimate/comparator disagreement
    async command REPAIR(mode: RepairMode)
    @ E9: periodic blind scrub of every neuron's golden configuration (0 = off)
    async command SCRUB_PERIOD(seconds: U16)
    @ E9 TEST ONLY: corrupt one configuration byte without updating the software copy
    @ (kind 0 = Cf bit 7: gain about x2, hold intact; kind 1 = C_V1 bit 6: hold broken)
    async command UPSET($neuron: U8, $kind: U8)
    @ E9 TEST ONLY: corrupt the software evidence estimate of one neuron
    async command CORRUPT_EST($neuron: U8, $value: F32)
    @ E10: chip-in-the-loop training on a labeled episode file; weights rewritten live (state kept)
    async command TRAIN(path: string size 40, epochs: U8, eta: F32)
    @ E11: frozen-weight inference on an episode file (trained = learned weights, else w = 0.1)
    async command INFER(path: string size 40, trained: bool)

    @ v7: autonomous fault protection (reads /home/amun/f/amun.cfg); survives restarts via warm start
    async command AUTONOMY(enable: bool)
    @ v7 TEST: start a workload behaviour now (param: ms/tick growth, ms, KB/tick, KB/tick, seconds)
    async command TASK($kind: TaskKind, $param: U32)

    # ---------------- events ----------------
    event Booted(posMask: U8, frames: U32) severity activity high \
      format "Neurons booted: polarity mask 0x{x}, {} frames, all neurons zeroed and biased"
    event HwError(what: string size 80, err: I32) severity warning high \
      format "Hardware error: {} ({})"
    event NotReady severity warning low format "Send BOOT first"
    event BadArg(what: string size 40) severity warning low format "Bad argument: {}"
    event WeightsSet($neuron: U8, cf: U8, cV2: U8, cV1: U8) severity activity low \
      format "Neuron {} weights: Cf={} C_V2={} C_V1={}"
    event TrialResult($neuron: U8, wNew: F32, tRunUs: U32, txUs: U32, ratio: F32) \
      severity activity high \
      format "Carry trial neuron {}: wNew={f} T={}us t_x={}us ratio={f}"
    event TrialNoCross($neuron: U8) severity warning high format "Carry trial neuron {}: no crossing"
    event GpsDegraded($neuron: U8, feature: string size 16) severity warning high \
      format "Neuron {} fired: GPS {} degraded"
    event GpsRecovered($neuron: U8, feature: string size 16) severity activity high \
      format "Neuron {} released: GPS {} nominal"
    event GpsTrust(trusted: bool) severity activity high format "GPS trusted = {}"
    event ModeChanged(mode: NeuroMode, wiped: bool) severity activity high \
      format "Mode {} (evidence wiped = {})"
    event Charged(tMs: U16) severity activity high format "All neurons charged with +1 for {} ms (checkpoint written)"
    event Readout($neuron: U8, $state: F32, charged: F32, heldS: F32, drift: F32, ratioRaw: F32, ratioCorr: F32) \
      severity activity high \
      format "Readout neuron {}: state={f} charged={f} held={f}s drift={f}/s raw={f} corrected={f}"
    event ReadoutRail($neuron: U8, $state: F32) severity warning high \
      format "Readout neuron {}: state={f} at the rail (saturated, drift correction invalid)"
    event ReadoutNoCross($neuron: U8) severity warning high format "Readout neuron {}: no crossing"
    event Reattached(posMask: U8, heldS: F32, writeHold: bool, aboveMask: U8) severity activity high \
      format "Reattached without reset: polarity 0x{x}, {f}s since charge, HOLD rewritten = {}, above threshold 0x{x}"
    event Crashing(inRun: bool) severity warning high format "TEST: flight software aborting now (in RUN = {})"
    event LeakMeasured($neuron: U8, unitsPerS: F32) severity activity high \
      format "Neuron {} hold drift {f} units/s at w=0.1 (3 s hold at BOOT, compensated in MONITOR)"
    event Injected($neuron: U8, pct: I8, seconds: U16) severity warning low \
      format "TEST INJECTION neuron {}: {}% for {} s"

    event Recovered($neuron: U8, src: RecoverSrc, analogRaw: F32, analogCorr: F32, ckpt: F32, ckptAgeS: F32, \
                    downS: F32, applied: F32, rail: bool) severity activity high \
      format "Recover neuron {}: src={} analog raw={f} corrected={f} checkpoint={f} age={f}s down={f}s applied={f} rail={}"
    event Disagree($neuron: U8, est: F32, above: bool) severity warning high \
      format "Neuron {}: software estimate {f} disagrees with analog comparator (above threshold = {})"
    event Repaired($neuron: U8, how: string size 24, before: F32, after: F32) severity warning high \
      format "Neuron {} repaired by {}: estimate {f} -> {f}"
    event Upset($neuron: U8, $kind: U8, before: U8, after: U8) severity warning low \
      format "TEST UPSET neuron {} kind {}: config byte 0x{x} -> 0x{x}"
    event CorruptedEst($neuron: U8, before: F32, after: F32) severity warning low \
      format "TEST CORRUPT neuron {} software estimate {f} -> {f}"
    event Scrubbed(count: U32) severity activity low format "Scrub {}: golden configuration rewritten on all neurons"
    event TrainUpdate($neuron: U8, why: string size 16, wOld: F32, wNew: F32) severity activity low \
      format "Train neuron {}: {} -> weight {f} -> {f}"
    event TrainEpoch(ep: U8, falseAlarms: U16, misses: U16, w1: F32, w2: F32, w3: F32, w4: F32) \
      severity activity high \
      format "Train epoch {}: false alarms {} misses {} -> weights {f} {f} {f} {f}"
    event InferNeuron($neuron: U8, trained: bool, faults: U16, detected: U16, meanLat: F32, falseAlarms: U16, \
                      nominalEps: U16) severity activity high \
      format "Infer neuron {} trained={}: faults {} detected {} mean latency {f} steps, false alarms {} in {} nominal episodes"
    event InferSummary(trained: bool, faults: U16, detected: U16, meanLat: F32, falseAlarms: U16, episodes: U16) \
      severity activity high \
      format "Infer trained={}: faults {} detected {} mean latency {f} steps, false alarms {}, {} episodes"

    event Assigned(timing: U8, memory: U8, gps: U8, clk: U8) severity activity high \
      format "Monitors assigned by retention: TIMING->n{} MEMORY->n{} GPS-LINK->n{} CLOCK->n{}"
    event Baseline(mon: string size 12, off: F32, scale: F32) severity activity high \
      format "Baseline {}: offset {f} scale {f}"
    event Warn(mon: string size 12, x: F32, est: F32) severity warning low \
      format "WARN {}: evidence {f} membrane {f}"
    event Shed(mon: string size 12, execMs: F32, growKB: F32) severity warning high \
      format "SHED {}: non-essential work stopped (exec {f} ms, growth {f} KB/tick)"
    event Probe(mon: string size 12, justified: bool) severity activity high \
      format "PROBE {}: shed justified = {}"
    event Learned(mon: string size 12, why: string size 16, wOld: F32, wNew: F32, bOld: F32, bNew: F32) \
      severity activity high \
      format "LEARN {} ({}): gain {f} -> {f}, floor {f} -> {f}"
    event Death(downS: F32, clockS: F32, cause: string size 12, progress: F32, policy: string size 8) \
      severity warning high \
      format "DEATH detected at warm start: down {f} s (analog clock {f} s), cause {} (progress {f}), policy {}"
    event Escalated(cause: string size 12, policy: string size 8, deaths: U32) severity warning high \
      format "ESCALATE to SAFE: cause {}, policy {}, deaths in window {}"
    event WarmStart(policy: string size 8, safe: bool, faults: U8) severity activity high \
      format "Warm start: policy {}, SAFE = {}, persisted faults loaded {}"
    event AuditFix($neuron: U8, est: F32, analog: F32) severity warning low \
      format "AUDIT neuron {}: estimate {f} -> analog {f}"
    event TaskEvent(what: string size 16, $param: U32) severity activity low format "TASK {} {}"
    event OomKill(rssKB: U32) severity warning high format "Memory limit reached ({} KB): process killed (simulated OOM)"

    # ---------------- telemetry ----------------
    telemetry FiredMask: U8
    telemetry GpsFix: U8
    telemetry GpsSats: U8
    telemetry GpsHdop: F32
    telemetry GpsTrusted: bool
    telemetry Est1: F32
    telemetry Est2: F32
    telemetry Est3: F32
    telemetry Est4: F32
    telemetry FramesSent: U32
    telemetry ErrOk: bool

    time get port timeCaller
    import Fw.Command
    import Fw.Event
    import Fw.Channel
  }
}

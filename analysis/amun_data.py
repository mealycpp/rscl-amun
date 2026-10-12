"""All plotted numbers, copied from the raw F' event logs (v5 E8, v7.1/v7.2 E12, v7.2 E13).
Units: 1 unit = 1 ms of +1 input at w = 0.1 (about 66.8 mV)."""
# ---- E8: raw membrane state (units) after charge 15 and hold t (s), no-crash references ----
REF = {  # t: {neuron: [state,...]}
 17.0: {1:[15.745,16.243,16.316], 2:[-15.020,-15.218,-15.231], 3:[0.374,0.752,0.345], 4:[-32.698,-32.350,-33.060]},
 20.0: {1:[16.200,16.695,16.110], 2:[-20.874,-20.529,-20.604], 3:[-2.629,-2.464,-2.058], 4:[-41.367,-41.442,-41.696]},
 31.0: {1:[16.461], 2:[-43.275], 3:[-11.769], 4:[None]},       # n4 at rail (n2 -43.3 = 15 - 1.881*31, not railed)
}
# crash runs: (hold s, HOLD bytes rewritten?, {neuron: state}); None = rail
CRASH = [
 (20.0, True,  {1:16.544, 2:-21.218, 3:-2.492, 4:-40.930}),
 (17.3, False, {1:16.402, 2:-15.497, 3:0.131,  4:-34.369}),
 (27.5, True,  {1:16.106, 2:-35.360, 3:-8.715, 4:None}),
 (16.0, False, {1:15.852, 2:-13.326, 3:0.721,  4:-29.981}),
]
# no-crash reference interpolated to the same hold time
def ref_at(n, t):
    """No-crash reference at hold t: linear interpolation of reference means; below 17 s, 17-20 s slope."""
    import statistics as st
    pts = [(tt, st.mean(v)) for tt in sorted(REF) for v in [[x for x in REF[tt][n] if x is not None]] if v]
    for (t0, m0), (t1, m1) in zip(pts, pts[1:]):
        if t0 <= t <= t1:
            return m0 + (m1 - m0) * (t - t0) / (t1 - t0)
    (t0, m0), (t1, m1) = pts[0], pts[1]
    return m0 + (m1 - m0) * (t - t0) / (t1 - t0)
# crash minus reference, per neuron: list of (delta, HOLD rewritten?)
DELTA = {n: [(v[n] - ref_at(n, t), rew) for t, rew, v in CRASH if v[n] is not None] for n in (1, 2, 3, 4)}
N1_LONG = (116.5, 20.470)    # n1 crash run, corrected ratio 1.025
RAIL = -45.3
DRIFT = {1:-0.044, 2:1.881, 3:0.985, 4:2.794}   # units/s, BOOT of the E8 session

# ---- E12 (v7.2 final): watchdog kill times (s since first kill) and outcome ----
E12 = {
 'B0':        ([0.0, 11.40, 22.80, 34.20, 45.60, 57.00], 'cap'),
 'B1 (K=3)':  ([0.0, 8.28, 19.69], 'safe'),
 'B2, 5 s*':  ([0.0, 11.60, 23.00], 'safe'),   # v7.1 run
 'B2, 5 s':   ([0.0], 'safe'),
 'B2, 1 s':   ([0.0], 'safe'),
 'AMUN':      ([0.0], 'safe'),
}
# v7.1 B2 5 s run: TIMING evidence at each death (units). Threshold 0, floor -18.
B2_ANALOG = [2.853, 2.400, 7.500]
B2_CKPT   = [-18.000, -13.179, -8.322]
# E14 analog clock: (heartbeat downtime s, analog clock s), v7.2 E12, valid points only
CLOCK = [(5.068,5.225),(5.097,4.883),(5.175,5.037),(5.179,4.997),(5.179,5.019),(5.178,4.970),
         (2.032,2.068),(5.183,5.028),(5.109,5.322)]
# deaths until loop broken vs evidence NV writes per hour
PARETO = {'AMUN':(0,1,1), 'B2, 1 s':(3600,1,1), 'B2, 5 s':(720,1,3), 'B1 (K=3)':(0,3,3), 'B0':(0,6,6)}
# ---- E13 mis-tuned start (w0 = 0.06), deaths per third of the 12-min run (amun_e13.sh windows) ----
E13_ON  = [1,1,1]
E13_OFF = [2,4,2]
E13_GAIN = [('TIMING',0.060,0.069),('MEMORY',0.060,0.069),('TIMING',0.069,0.079)]

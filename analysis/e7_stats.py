#!/usr/bin/env python3
"""E7 numbers in Sec. IV, recomputed from the F' TrialResult events (data/e7_trials.txt,
one line per neuron: time nK w=.. T=<charge us> tx=<discharge us> r=tx/T).
Blocks (one CARRY_TRIAL command = 4 lines): 0-8 earlier run (9 commands, 20:52),
9-20 the `e7` step (3 runs x 4 gains, 20:57-20:58), 21-28 the `ctrl` step
(restore, plain, ... per amun_runs.sh; restore trials log w=0.1), 29 a stray w=0.1 trial."""
import re, os, statistics as st
from datetime import datetime
f = os.path.join(os.path.dirname(__file__), "..", "data", "e7_trials.txt")
rows = []
for line in open(f):
    m = re.match(r'(\S+) n(\d) w=([\d.]+) T=(\d+) tx=(\d+) r=([\d.]+)', line.strip())
    if m:
        rows.append((datetime.fromisoformat(m.group(1)), int(m.group(2)), float(m.group(3)), float(m.group(6))))
blocks, cur = [], [rows[0]]
for r in rows[1:]:
    if (r[0] - cur[-1][0]).total_seconds() < 0.5: cur.append(r)
    else: blocks.append(cur); cur = [r]
blocks.append(cur)
assert len(blocks) == 30 and all(len(b) == 4 for b in blocks)
S1, S2, CT = blocks[0:9], blocks[9:21], blocks[21:29]
M = lambda S, w, k=None: st.median([x[3] for b in S for x in b if x[2] == w and (k is None or x[1] == k)])
G = (0.05, 0.1, 0.2, 0.4)
print("pooled medians (48):", [round(M(S2, w), 3) for w in G])
print("earlier run max diff:", round(max(abs(M(S1, w) - M(S2, w)) for w in G), 4))
print("w=0.1 per neuron:", {k: round(M(S2, 0.1, k), 3) for k in (1, 2, 3, 4)})
dev = max(abs(M(S2, w, k) / M(S2, 0.1, k) / (0.1 / w) - 1) for k in (1, 2, 3, 4) for w in G)
print("largest normalized deviation of per-neuron medians: %.2f %%" % (100 * dev))
R, P = CT[0::2], CT[1::2]
for k in (1, 2, 3, 4):
    rk = [x[3] for b in R for x in b if x[1] == k]; pk = [x[3] for b in P for x in b if x[1] == k]
    print("ctrl n%d: median shift %+.4f, plain range %.4f" % (k, st.median(rk) - st.median(pk), max(pk) - min(pk)))

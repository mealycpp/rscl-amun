#!/usr/bin/env python3
"""Cut the FNIRSI meter log into the states marked by amun_power.sh and print mean power per state.
usage: amun_power_cut.py meter.txt steps.txt [trim_start_s] [trim_end_s]"""
import sys, re, statistics as st

meter, steps = sys.argv[1], sys.argv[2]
t_in = float(sys.argv[3]) if len(sys.argv) > 3 else 10.0    # skip the first seconds of each state
t_out = float(sys.argv[4]) if len(sys.argv) > 4 else 5.0    # and the last seconds

S = []                                       # (t, V, I)
for line in open(meter, errors='replace'):
    f = line.split()
    if len(f) >= 4 and re.match(r'^\d+\.\d+$', f[0]):
        try:
            S.append((float(f[0]), float(f[2]), float(f[3])))
        except ValueError:
            pass

M = {}
for line in open(steps, errors='replace'):
    m = re.match(r'^(\d+\.\d+) \S+ \S+ (S\d) (start|end)', line)
    if m:
        M.setdefault(m.group(2), {})[m.group(3)] = float(m.group(1))

print(f"meter samples {len(S)}; states trimmed {t_in:g} s at start, {t_out:g} s at end")
R = {}
for k in sorted(M):
    a, b = M[k].get('start'), M[k].get('end')
    if a is None or b is None:
        print(f"{k}: incomplete marks"); continue
    w = [(v, i) for t, v, i in S if a + t_in <= t <= b - t_out]
    if len(w) < 100:
        print(f"{k}: only {len(w)} samples"); continue
    p = [v * i * 1000 for v, i in w]                       # mW
    # 1 s block means give an honest spread (samples within a second are correlated)
    blocks = [st.mean(p[j:j + 100]) for j in range(0, len(p) - 99, 100)]
    R[k] = st.mean(p)
    print(f"{k}: n={len(w)}  V={st.mean(v for v, _ in w):.4f} V (min {min(v for v, _ in w):.4f})  "
          f"I={1000 * st.mean(i for _, i in w):.1f} mA  P={R[k]:.1f} mW  "
          f"1 s means: sd {st.pstdev(blocks):.1f} mW, range {min(blocks):.1f}..{max(blocks):.1f}")

def d(x, y, name):
    if x in R and y in R:
        print(f"{x} - {y} = {R[x] - R[y]:+.1f} mW   ({name})")
d('S3', 'S1', 'four configured FPAAs')
d('S2', 'S3', "F' running AMUN with GDS, on top of configured FPAAs")
d('S2', 'S1', 'whole AMUN setup over idle board')

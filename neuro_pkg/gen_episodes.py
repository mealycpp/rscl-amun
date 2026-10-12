#!/usr/bin/env python3
"""Labeled GPS episodes for AMUN training (E10) and inference (E11).

Each episode is 30 one-step samples of the four F' monitor features
(fix, satellites, HDOP, position jump). One third are nominal; the rest carry
one labeled fault on one feature. Nominal data includes realistic nuisance:
satellite count wandering 6-11, HDOP multipath spikes (3.0-4.2 for 3-5 steps),
and isolated single-step position glitches.

Line format: ep,mask,fix,sats,hdop,jump  (mask bit k = neuron k+1 should fire)
usage: python3 gen_episodes.py out.csv seed [episodes]
"""
import random
import sys

STEPS = 30


def episode(rng, ep, kind):
    rows = []
    sats = rng.randint(7, 10)
    spike = None
    if rng.random() < 0.45:                       # multipath HDOP spike (nuisance, not a fault)
        s0 = rng.randint(3, STEPS - 6)
        spike = (s0, s0 + rng.randint(3, 5), rng.uniform(3.0, 4.2))
    glitch = rng.randint(2, STEPS - 2) if rng.random() < 0.35 else None   # single-step jump glitch
    onset = rng.randint(8, 14)
    end = onset + rng.randint(8, 14)
    for s in range(STEPS):
        sats = min(11, max(6, sats + rng.choice((-1, 0, 0, 1))))
        fix, hd, jump = 1, rng.uniform(1.0, 2.4), 0
        if spike and spike[0] <= s < spike[1]:
            hd = spike[2] + rng.uniform(-0.2, 0.2)
        if glitch == s:
            jump = 1
        sat_s = sats
        fault = kind is not None and onset <= s < end
        mask = 0
        if fault:
            if kind == 0:
                fix = 0                                     # outage
            elif kind == 1:
                sat_s = rng.choice((4, 5, 5))               # weak geometry: few satellites
            elif kind == 2:
                hd = rng.uniform(4.5, 8.0)                   # degraded dilution of precision
            elif kind == 3:
                jump = 1 if rng.random() < 0.85 else 0       # sustained implausible jumps (spoofing)
            mask = 1 << kind
        rows.append(f"{ep},{mask},{fix},{sat_s},{hd:.2f},{jump}")
    return rows


def main():
    out, seed = sys.argv[1], int(sys.argv[2])
    n = int(sys.argv[3]) if len(sys.argv) > 3 else 48
    rng = random.Random(seed)
    kinds = [None] * (n // 3) + [i % 4 for i in range(n - n // 3)]
    rng.shuffle(kinds)
    with open(out, "w") as f:
        f.write("# ep,mask,fix,sats,hdop,jump  seed=%d\n" % seed)
        for ep, kind in enumerate(kinds, 1):
            f.write("\n".join(episode(rng, ep, kind)) + "\n")
    print(f"{out}: {n} episodes ({kinds.count(None)} nominal), {n * STEPS} steps")


if __name__ == "__main__":
    main()

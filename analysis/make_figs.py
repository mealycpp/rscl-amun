#!/usr/bin/env python3
"""Figs. 2-4 for the AMUN paper. Vector PDF, TrueType (Type 42) fonts, no Type 3."""
import statistics as st
import sys, os
import matplotlib
matplotlib.use("pdf")
import matplotlib.pyplot as plt
from matplotlib import font_manager as fm
sys.path.insert(0, os.path.dirname(__file__))
from amun_data import *

for f in ("texgyreheros-regular.otf", "texgyreheros-bold.otf"):
    p = "/usr/share/texmf/fonts/opentype/public/tex-gyre/" + f
    if os.path.exists(p):
        fm.fontManager.addfont(p)
plt.rcParams.update({
    "font.family": "Liberation Sans", "font.size": 7, "axes.labelsize": 7, "axes.titlesize": 7,
    "xtick.labelsize": 6.5, "ytick.labelsize": 6.5, "legend.fontsize": 6.5,
    "pdf.fonttype": 42, "ps.fonttype": 42, "axes.linewidth": 0.6,
    "xtick.major.width": 0.6, "ytick.major.width": 0.6, "xtick.major.size": 2.5, "ytick.major.size": 2.5,
    "axes.spines.top": False, "axes.spines.right": False, "lines.linewidth": 1.2,
    "axes.grid": True, "grid.color": "#e3e3e3", "grid.linewidth": 0.5, "axes.axisbelow": True,
    "mathtext.fontset": "custom", "mathtext.rm": "Liberation Sans", "mathtext.it": "Liberation Sans:italic", "mathtext.sf": "Liberation Sans",
})
INK, MUTED = "#282828", "#8a8a8a"
NC = {1: "#0072B2", 2: "#D55E00", 3: "#009E73", 4: "#CC79A7"}     # validated, CVD floor -> markers differ too
NM = {1: "o", 2: "s", 3: "^", 4: "D"}
ROLE = {1: "n1", 2: "n2", 3: "n3", 4: "n4"}
AMUN_C, CKPT_C, BASE_C = "#009E73", "#0072B2", "#D55E00"
OUT = os.path.join(os.path.dirname(__file__), "..", "fig")
W1 = 3.45  # single column width (in)


def tag(ax, s):
    ax.text(-0.02, 1.03, s, transform=ax.transAxes, ha="right", va="bottom", fontweight="bold", fontsize=7.5)


def ylab(ax, s):   # horizontal y label above the axis (no rotated text)
    ax.text(0.0, 1.03, s, transform=ax.transAxes, ha="left", va="bottom", fontsize=6.5)


# ---------------- Fig. 2: state survives flight-software death (E8) ----------------
def fig2():
    fig, (a, b) = plt.subplots(1, 2, figsize=(W1, 1.32), gridspec_kw={"width_ratios": [1.55, 1]})
    for n in (1, 2, 3, 4):
        # no-crash references (mean of 3 runs at 17 s and 20 s; single run at 31 s)
        ts, ms = [], []
        for t in sorted(REF):
            v = [x for x in REF[t][n] if x is not None]
            if v:
                ts.append(t); ms.append(st.mean(v))
        # BOOT-measured drift line anchored at the 17 s reference, clipped at the rail
        t0, m0 = ts[0], ms[0]
        tl = [14.5, 33.0]
        yl = [max(RAIL, m0 - DRIFT[n] * (t - t0)) for t in tl]
        if yl[1] == RAIL:
            tr = t0 + (m0 - RAIL) / DRIFT[n]; tl = [14.5, tr, 33.0]; yl = [m0 - DRIFT[n] * (14.5 - t0), RAIL, RAIL]
        a.plot(tl, yl, color=NC[n], lw=0.8, alpha=0.55, zorder=1)
        a.plot(ts, ms, ls="none", marker="_", ms=7, mew=1.2, color=MUTED, zorder=2)
        for hold, rew, v in CRASH:
            if v[n] is None:
                continue
            a.plot(hold, v[n], ls="none", marker=NM[n], ms=4, mew=0.8, color=NC[n],
                   mfc=NC[n] if rew else "white", zorder=3)
    a.axhline(RAIL, color=MUTED, lw=0.6, ls=(0, (3, 2)))
    a.text(15.0, RAIL + 1.8, "rail", ha="left", va="bottom", color=MUTED, fontsize=6)
    for n, (x, y) in {1: (32.6, 20.6), 3: (32.6, -4.5), 2: (22.6, -19.0), 4: (22.2, -39.5)}.items():
        a.text(x, y, ROLE[n], color=INK, fontsize=6, ha="right" if n in (1, 3) else "left", va="center")
    a.set_xlim(14.5, 33); a.set_ylim(-48, 22); a.set_yticks([-40, -20, 0, 20])
    a.set_xlabel("Hold time (s)", labelpad=1.5); ylab(a, "$x$ (units)")
    tag(a, "(a)")
    # (b) crash minus matched reference
    for n in (1, 2, 3, 4):
        sds = [st.stdev(REF[t][n]) for t in (17.0, 20.0)]
        sd = max(sds)
        b.add_patch(plt.Rectangle((n - 0.32, -sd), 0.64, 2 * sd, color="#ececec", lw=0, zorder=0))
        for i, (d, r) in enumerate(DELTA[n]):
            b.plot(n + (i - 1.5) * 0.12, d, ls="none", marker=NM[n], ms=4, mew=0.8, color=NC[n],
                   mfc=NC[n] if r else "white", zorder=3)
    b.axhline(0, color=INK, lw=0.6)
    b.set_xticks([1, 2, 3, 4]); b.set_xticklabels(["n1", "n2", "n3", "n4"])
    b.set_ylim(-1.2, 1.2); b.set_xlim(0.45, 4.55)
    ylab(b, "crash − reference (units)")
    b.grid(axis="x", visible=False)
    tag(b, "(b)")
    from matplotlib.lines import Line2D
    h = [Line2D([], [], ls="none", marker="o", ms=4, color=INK, mfc=INK, mew=0.8),
         Line2D([], [], ls="none", marker="o", ms=4, color=INK, mfc="white", mew=0.8),
         Line2D([], [], ls="none", marker="_", ms=7, mew=1.2, color=MUTED)]
    fig.legend(h, ["after crash, HOLD rewritten", "after crash, 0 writes", "no-crash reference"],
               loc="lower center", ncol=3, frameon=False, handletextpad=0.2, columnspacing=0.9,
               bbox_to_anchor=(0.5, -0.02))
    fig.subplots_adjust(left=0.085, right=0.985, top=0.851, bottom=0.338, wspace=0.3)
    fig.savefig(os.path.join(OUT, "fig2_e8.pdf"))


# ---------------- Fig. 3: reset loop (E12) ----------------
def fig3():
    fig, (a, b) = plt.subplots(1, 2, figsize=(W1, 1.27), gridspec_kw={"width_ratios": [1.6, 1]})
    names = list(E12.keys())
    for i, nm in enumerate(names):
        y = len(names) - 1 - i
        ts, end = E12[nm]
        c = AMUN_C if nm == "AMUN" else (CKPT_C if nm.startswith("B2") else BASE_C if nm != "B0" else MUTED)
        last = ts[-1]
        a.plot([0, last + (2.1 if end == "safe" else 6)], [y, y], color=c, lw=0.8, alpha=0.5)
        a.plot(ts, [y] * len(ts), ls="none", marker="x", ms=4.5, mew=1.1, color=c)
        if end == "safe":
            a.plot(last + 2.1, y, marker="s", ms=4, color=c, mew=0)
        else:
            a.annotate("", xy=(last + 9, y), xytext=(last + 4, y),
                       arrowprops=dict(arrowstyle="->", color=c, lw=0.8))
        a.text(last + (4.6 if end == "safe" else 10.5), y, f"{len(ts)}" if end == "safe" else f"\u2265{len(ts)}", va="center", fontsize=6,
               color=INK)
    a.set_yticks(range(len(names))); a.set_yticklabels(names[::-1])
    a.set_xlim(-3, 72); a.set_ylim(-0.6, len(names) - 0.4)
    a.set_xlabel("Time since first death (s)")
    a.grid(axis="y", visible=False)
    from matplotlib.lines import Line2D
    a.legend([Line2D([], [], ls="none", marker="x", ms=4.5, mew=1.1, color=INK),
              Line2D([], [], ls="none", marker="s", ms=4, color=INK)],
             ["watchdog kill", "SAFE, loop broken"], loc="lower right", frameon=False,
             handletextpad=0.2, borderaxespad=0.1)
    tag(a, "(a)")
    # (b) evidence at each death, B2 5 s (earlier run)
    k = [1, 2, 3]
    b.axhline(0, color=INK, lw=0.6)
    b.axhline(-18, color=MUTED, lw=0.6, ls=(0, (3, 2)))
    b.text(4.55, 0.6, "threshold", ha="right", va="bottom", fontsize=6, color=INK)
    b.text(4.55, -17.4, "floor", ha="right", va="bottom", fontsize=6, color=MUTED)
    for i in range(3):
        b.plot([k[i], k[i]], [B2_CKPT[i], B2_ANALOG[i]], color="#bdbdbd", lw=0.8, zorder=1)
    b.plot(k, B2_ANALOG, ls="none", marker="o", ms=4.5, color=AMUN_C, label="analog charge", zorder=3)
    b.plot(k, B2_CKPT, ls="none", marker="s", ms=4.2, color=CKPT_C, label="5 s checkpoint", zorder=3)
    b.set_xticks(k); b.set_xlim(0.5, 4.6); b.set_ylim(-21, 25); b.set_yticks([-20, -10, 0, 10])
    b.set_xlabel("Death"); ylab(b, "TIMING (units)")
    b.legend(loc="upper left", frameon=False, handletextpad=0.2, borderaxespad=0.1)
    b.grid(axis="x", visible=False)
    tag(b, "(b)")
    fig.subplots_adjust(left=0.165, right=0.985, top=0.848, bottom=0.265, wspace=0.3)
    fig.savefig(os.path.join(OUT, "fig3_e12.pdf"))


# ---------------- Fig. 4: comparison and learning ----------------
def fig4():
    fig, (a, b) = plt.subplots(1, 2, figsize=(W1, 1.22), gridspec_kw={"width_ratios": [1.25, 1]})
    style = {"AMUN": (AMUN_C, "o"), "B2, 1 s": (CKPT_C, "s"), "B2, 5 s": (CKPT_C, "D"),
             "B1 (K=3)": (BASE_C, "^"), "B0": (MUTED, "o")}
    off = {"AMUN": (6, 0, "left"), "B2, 1 s": (0, 8, "center"), "B2, 5 s": (0, 8, "center"),
           "B1 (K=3)": (6, 0, "left"), "B0": (6, 0, "left")}
    for nm, (x, lo, hi) in PARETO.items():
        c, m = style[nm]
        if hi > lo:
            a.plot([x, x], [lo, hi], color=c, lw=1.0)
        a.plot(x, lo if nm != "B2, 5 s" else lo, ls="none", marker=m, ms=4.5, color=c, zorder=3)
        if nm == "B2, 5 s":
            a.plot(x, hi, ls="none", marker=m, ms=4.5, color=c, mfc="white", zorder=3)
        if nm == "B0":
            a.annotate("", xy=(x, 7.4), xytext=(x, 6.25), arrowprops=dict(arrowstyle="->", color=c, lw=0.8))
        dx, dy, ha = off[nm]
        a.annotate({"B0": "B0 (capped)", "B1 (K=3)": "B1 (+1 per boot)"}.get(nm, nm), (x, hi if nm in ("B2, 5 s",) else lo), xytext=(dx, dy), textcoords="offset points",
                   fontsize=6, ha=ha, va="center", color=INK)
    a.set_xscale("symlog", linthresh=1, linscale=0.6)
    a.set_xlim(-0.4, 2e4); a.set_ylim(0, 7.6)
    a.set_xticks([0, 1, 10, 100, 1000, 10000]); a.set_xticklabels(["0", "1", "10", "$10^2$", "$10^3$", "$10^4$"])
    a.set_yticks([1, 2, 3, 4, 5, 6])
    a.set_xlabel("Evidence NV writes per fault-free hour"); ylab(a, "deaths until loop broken")
    a.minorticks_off()
    tag(a, "(a)")
    import numpy as np
    x = np.arange(3); w = 0.36
    b.bar(x - w / 2 - 0.01, E13_OFF, w, color=BASE_C, label="learning OFF")
    b.bar(x + w / 2 + 0.01, E13_ON, w, color=AMUN_C, label="learning ON")
    b.set_xticks(x); b.set_xticklabels(["1st", "2nd", "3rd"])
    b.set_xlabel("Third of the 12-min run"); ylab(b, "deaths")
    b.set_ylim(0, 6.4); b.set_yticks([0, 1, 2, 3, 4, 5])
    b.legend(loc="upper right", frameon=False, handlelength=1.0, handletextpad=0.3, borderaxespad=0.1)
    b.grid(axis="x", visible=False)
    tag(b, "(b)")
    fig.subplots_adjust(left=0.07, right=0.985, top=0.848, bottom=0.28, wspace=0.3)
    fig.savefig(os.path.join(OUT, "fig4_cmp.pdf"))


if __name__ == "__main__":
    fig2(); fig3(); fig4(); print("ok")

"""Plot the default tonal-zone windows (brief 7.1, 11.6 "plot weight versus
input stops"). Usage:

    ./test_color --dump-zones > zones.txt
    python3 tests/cpu/plot_zones.py zones.txt docs/images/zone-windows.png [gray_nits]

The axis runs from -9 stops (below that is camera noise; Black and Dark carry on
down to true black) to OBS's HLG encode ceiling, 10-bit code 1023.
"""
import sys
import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

d = np.loadtxt(sys.argv[1])
gray = float(sys.argv[3]) if len(sys.argv) > 3 else 18.0
names = ["Black", "Dark", "Shadow", "Midtones", "Light", "Highlight", "Specular"]
colors = ["#3b4cc0", "#2a9df4", "#1a9850", "#7f7f7f", "#d9b500", "#f46d43", "#c51b7d"]


def hlg_pct(nits):
    # OBS HLG encode for a <= 1000-nit peak: nits/1000 -> inverse OOTF (Yd^(-1/6) on neutrals) -> OETF
    e = np.clip(nits / 1000.0, 0, None)
    e = np.where(e > 0, e ** (1 / 1.2), 0)
    a, b, c = 0.17883277, 0.28466892, 0.55991073
    return 100 * np.where(e <= 1 / 12, np.sqrt(3 * e), a * np.log(np.maximum(12 * e - b, 1e-12)) + c)


fig, ax = plt.subplots(figsize=(11, 4.6), dpi=130)
for i, n in enumerate(names):
    ax.plot(d[:, 0], d[:, i + 1], color=colors[i], lw=2, label=n)
    ax.fill_between(d[:, 0], 0, d[:, i + 1], color=colors[i], alpha=0.08)
# Right edge: the stop where OBS's HLG encode reaches 10-bit code 1023 (E' = 959/876).
# Linear values above it exist inside the filter (and the zones act on them), but
# they cannot appear in the encoded output, so the plot stops there.
lo_n, hi_n = 1000.0, 3000.0
for _ in range(100):
    mid = 0.5 * (lo_n + hi_n)
    lo_n, hi_n = (mid, hi_n) if 64 + 8.76 * hlg_pct(mid) < 1023 else (lo_n, mid)
ceiling_nits = lo_n
ceiling_stop = np.log2(ceiling_nits / gray)
LEFT = -9.0
ticks = [-9, -7, -5, -4, -3, -1, 0, 1, 2, 3, 4, 5, ceiling_stop]
ax.set_xticks(ticks)


def nits_label(nits):
    # plain decimals, no exponent notation
    if nits >= 10:
        return f"{nits:.0f}"
    if nits >= 1:
        return f"{nits:.1f}"
    digits = 1
    while nits * 10 ** digits < 10:
        digits += 1
    return f"{nits:.{digits}f}"


def tick_label(t):
    n = gray * 2.0 ** t
    stop = f"{t:+.1f}" if abs(t - round(t)) > 1e-9 else f"{int(round(t)):+d}"
    return f"{stop}\n{nits_label(n)}\n{hlg_pct(n):.0f}%\n{min(1023, 64 + 8.76 * hlg_pct(n)):.0f}"


ax.set_xticklabels([tick_label(t) for t in ticks], fontsize=8)
ax.set_xlabel(f"stops from gray ({gray:g} nits)  /  nits  /  HLG % (1000-nit peak)  /  10-bit code (narrow range)", fontsize=9)
ax.set_ylabel("zone weight")
ax.set_ylim(-0.02, 1.08)
ax.set_xlim(LEFT, ceiling_stop)
ax.grid(alpha=0.25)
ax.legend(ncol=7, loc="upper center", fontsize=8, frameon=False, bbox_to_anchor=(0.5, 1.12))
fig.tight_layout()
fig.savefig(sys.argv[2])

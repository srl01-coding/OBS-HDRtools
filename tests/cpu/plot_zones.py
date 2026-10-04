"""Plot the default tonal-zone windows (brief 7.1, 11.6 "plot weight versus
input stops"). Usage:

    ./test_color --dump-zones > zones.txt
    python3 tests/cpu/plot_zones.py zones.txt zones.png [gray_nits]
"""
import sys
import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

d = np.loadtxt(sys.argv[1])
gray = float(sys.argv[3]) if len(sys.argv) > 3 else 18.0
names = ["Black", "Dark", "Shadow", "Light", "Highlight", "Specular"]
colors = ["#3b4cc0", "#2a9df4", "#1a9850", "#d9b500", "#f46d43", "#c51b7d"]


def hlg_pct(nits):
    # OBS HLG encode for a <= 1000-nit peak: nits/1000 -> inverse OOTF (Yd^(-1/6) on neutrals) -> OETF
    e = np.clip(nits / 1000.0, 0, None)
    e = np.where(e > 0, e ** (1 / 1.2), 0)
    a, b, c = 0.17883277, 0.28466892, 0.55991073
    return 100 * np.where(e <= 1 / 12, np.sqrt(3 * e), a * np.log(np.maximum(12 * e - b, 1e-12)) + c)


fig, ax = plt.subplots(figsize=(11, 4.2), dpi=130)
for i, n in enumerate(names):
    ax.plot(d[:, 0], d[:, i + 1], color=colors[i], lw=2, label=n)
    ax.fill_between(d[:, 0], 0, d[:, i + 1], color=colors[i], alpha=0.08)
ticks = np.arange(-12, 11, 2)
ax.set_xticks(ticks)
def hlg_label(nits):
    # OBS's HLG encoder tops out at code 1023 (E' = 1.0947, about 1866 nits at a 1000-nit peak)
    return ">109%" if nits > 1866 else f"{hlg_pct(nits):.0f}%"


ax.set_xticklabels([f"{t:+d}\n{gray * 2.0 ** t:.3g}\n{hlg_label(gray * 2.0 ** t)}" for t in ticks], fontsize=8)
ax.set_xlabel(f"stops from gray ({gray:g} nits)  /  nits  /  HLG % (1000-nit peak)", fontsize=9)
ax.set_ylabel("zone weight")
ax.set_ylim(-0.02, 1.08)
ax.set_xlim(d[0, 0], d[-1, 0])
ax.grid(alpha=0.25)
ax.legend(ncol=6, loc="upper center", fontsize=8, frameon=False, bbox_to_anchor=(0.5, 1.12))
fig.tight_layout()
fig.savefig(sys.argv[2])

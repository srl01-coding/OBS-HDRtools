"""Cross-check the C++ white-balance matrices against the brief's Python
reference (section 15, wb_matrix). Usage:

    ./test_color --dump-wb > wb.txt
    python3 tests/cpu/check_wb_vs_brief.py wb.txt
"""
import sys
import numpy as np

RGB_XYZ = np.array([[0.412390799265959, 0.357584339383878, 0.180480788401834],
                    [0.212639005871510, 0.715168678767756, 0.072192315360734],
                    [0.019330818715592, 0.119194779794626, 0.950532152249661]])
BRAD = np.array([[0.8951, 0.2664, -0.1614], [-0.7502, 1.7135, 0.0367], [0.0389, -0.0685, 1.0296]])


# --- functions copied verbatim from the brief, section 15 ---
def daylight_xy(t):
    if not 4000 <= t <= 25000: raise ValueError('temperature outside daylight domain')
    if t <= 7000: x = -4.6070e9/t**3+2.9678e6/t**2+0.09911e3/t+0.244063
    else: x = -2.0064e9/t**3+1.9018e6/t**2+0.24748e3/t+0.237040
    return np.array([x, -3*x*x+2.87*x-0.275])
def xy_uv(xy):
    x, y = xy; d = -2*x+12*y+3; return np.array([4*x/d, 6*y/d])
def uv_xy(uv):
    u, v = uv; d = 4+2*u-8*v; return np.array([3*u/d, 2*v/d])
def white_xyz(xy):
    x, y = xy; return np.array([x/y, 1, (1-x-y)/y])
def target_white(mired, tint):
    m0 = 1e6/6504
    def locus(m): return xy_uv(daylight_xy(1e6/m))
    uv0 = xy_uv([0.3127, 0.3290]); m = m0+mired
    uv = uv0+locus(m)-locus(m0)
    tangent = locus(m+0.1)-locus(m-0.1)
    normal = np.array([tangent[1], -tangent[0]]); normal /= np.linalg.norm(normal)
    if normal[1] > 0: normal = -normal
    return white_xyz(uv_xy(uv+tint*0.01*normal))
def wb_matrix(mired, tint):
    if mired == 0 and tint == 0: return np.eye(3)
    w0 = white_xyz([0.3127, 0.3290]); wt = target_white(mired, tint)
    a = np.linalg.inv(BRAD)@np.diag((BRAD@wt)/(BRAD@w0))@BRAD
    return np.linalg.inv(RGB_XYZ)@a@RGB_XYZ
# ------------------------------------------------------------

worst = 0.0
n = 0
for line in open(sys.argv[1]):
    vals = [float(v) for v in line.split()]
    m, t, mat = vals[0], vals[1], np.array(vals[2:]).reshape(3, 3)
    ref = wb_matrix(m, t / 100.0)
    worst = max(worst, float(np.max(np.abs(mat - ref))))
    n += 1
print(f"{n} white-balance matrices compared with the brief reference: max abs difference {worst:.3g}")
sys.exit(0 if n == 15 and worst < 1e-12 else 1)

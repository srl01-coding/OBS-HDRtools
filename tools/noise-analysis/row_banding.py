"""Row-structured share of the I-frame pair noise (banding / horizontal correlation test).
Run in the directory holding iframes.yuv and flat40.npy (from noise_I.py)."""
import numpy as np
from clipio import *
B = 40
flat = np.load('flat40.npy')
ys = [y.astype(np.float32) for i, y, u, v in frames('iframes.yuv', range(1, 9))]
for k in range(3):
    D = ys[k] - ys[k + 1]
    rv, pv, cv = [], [], []
    for by, bx in zip(*np.nonzero(flat)):
        blk = D[by * B:(by + 1) * B, bx * B:(bx + 1) * B]
        blk = blk - blk.mean()
        rv.append(blk.mean(1).var()); cv.append(blk.mean(0).var()); pv.append(blk.var())
    print('pair', k, 'row-mean var share %.3f, column-mean share %.3f (white noise: %.3f)'
          % (np.mean(rv) / np.mean(pv), np.mean(cv) / np.mean(pv), 1 / B))

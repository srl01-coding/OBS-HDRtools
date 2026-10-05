import numpy as np
W, H = 4560, 2160
FS = W * H * 3 // 2  # samples per frame (yuv420)
def frames(path, idx=None):
    mm = np.memmap(path, dtype='<u2', mode='r')
    n = mm.size // FS
    for i in (range(n) if idx is None else idx):
        f = mm[i * FS:(i + 1) * FS]
        y = f[:W * H].reshape(H, W)
        u = f[W * H:W * H + W * H // 4].reshape(H // 2, W // 2)
        v = f[W * H + W * H // 4:].reshape(H // 2, W // 2)
        yield i, y, u, v
def count(path):
    import os
    return os.path.getsize(path) // (FS * 2)

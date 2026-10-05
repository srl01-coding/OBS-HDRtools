import numpy as np
a, b, c = 0.17883277, 0.28466892, 0.55991073
def inv_oetf(e):
    e = np.asarray(e, dtype=np.float64)
    return np.where(e <= 0.5, e * e / 3.0, (np.exp((e - c) / a) + b) / 12.0)
def code_to_nits_grey(code):
    """Neutral grey: narrow-range 10-bit code -> display luminance, HLG at 1000 nits (gamma 1.2)."""
    e = (np.asarray(code, np.float64) - 64.0) / 876.0
    return 1000.0 * inv_oetf(np.clip(e, 0, None)) ** 1.2

#!/usr/bin/env python3
"""Same-bitrate HEVC Main10 test against a clean reference (denoise decision section 17).

    denoise_sim --export DIR object|pan      # writes clean.f32, m<k>.f32, methods.txt
    codec_test.py DIR [W H]

Each sequence (float32 linear RGB in nominal nits, Rec.709 primaries, OBS working
scale) is converted to HLG / BT.2020 / 10-bit narrow-range 4:2:0 exactly as an HLG
stream would carry it (inverse OOTF at 1000 nits, HLG OETF, BT.2020 NCL Y'CbCr). It is
then encoded with libx265 (Main10) at fixed average bitrates, decoded, and compared
with the *clean* reference encoded the same way (never with the noisy input):
PSNR and SSIM of the 10-bit luma codes. CRF size shows the bits spent on noise.
Requires ffmpeg with libx265, numpy, opencv-python.
"""
import os
import subprocess
import sys

import cv2
import numpy as np

A, B, C = 0.17883277, 0.28466892, 0.55991073
M709_TO_2020 = np.array([[0.6274, 0.3293, 0.0433], [0.0691, 0.9195, 0.0114], [0.0164, 0.0880, 0.8956]])
LW, GAMMA = 1000.0, 1.2


def oetf(e):
    e = np.clip(e, 0, None)
    return np.where(e <= 1 / 12, np.sqrt(3 * e), A * np.log(np.maximum(12 * e - B, 1e-12)) + C)


def nits_to_yuv420p10(rgb):
    """rgb: (H, W, 3) linear nits, Rec.709 primaries -> (Y, U, V) uint16 planes."""
    d = np.clip(rgb @ M709_TO_2020.T, 0, None)  # display light, BT.2020
    yd = d @ np.array([0.2627, 0.6780, 0.0593])
    ys = np.power(np.maximum(yd / LW, 1e-12), 1 / GAMMA)
    s = d / (LW * np.power(ys, GAMMA - 1))[..., None]  # inverse OOTF -> scene light
    e = oetf(s)
    y = e @ np.array([0.2627, 0.6780, 0.0593])
    cb = (e[..., 2] - y) / 1.8814
    cr = (e[..., 0] - y) / 1.4746
    Y = np.clip(np.round(64 + 876 * y), 0, 1023).astype(np.uint16)
    h, w = Y.shape
    cb2 = cb.reshape(h // 2, 2, w // 2, 2).mean(axis=(1, 3))
    cr2 = cr.reshape(h // 2, 2, w // 2, 2).mean(axis=(1, 3))
    U = np.clip(np.round(512 + 896 * cb2), 0, 1023).astype(np.uint16)
    V = np.clip(np.round(512 + 896 * cr2), 0, 1023).astype(np.uint16)
    return Y, U, V


def load_f32(path, w, h):
    a = np.fromfile(path, dtype=np.float32)
    return a.reshape(-1, h, w, 3).astype(np.float64)


def write_yuv(path, frames):
    with open(path, "wb") as f:
        for rgb in frames:
            for p in nits_to_yuv420p10(rgb):
                f.write(p.astype("<u2").tobytes())


def read_y(path, w, h):
    fs = w * h * 3 // 2
    a = np.fromfile(path, dtype="<u2")
    n = a.size // fs
    return [a[i * fs:i * fs + w * h].reshape(h, w).astype(np.float64) for i in range(n)]


def encode_decode(src, w, h, tmp, mode, value):
    out = tmp + ".hevc"
    dec = tmp + ".dec.yuv"
    if mode == "abr":
        xp = f"bitrate={value}:vbv-maxrate={value}:vbv-bufsize={2 * value}:keyint=60:min-keyint=60:log-level=error"
    else:
        xp = f"crf={value}:keyint=60:min-keyint=60:log-level=error"
    subprocess.run(["ffmpeg", "-v", "error", "-y", "-f", "rawvideo", "-pix_fmt", "yuv420p10le", "-s", f"{w}x{h}",
                    "-r", "30", "-i", src, "-c:v", "libx265", "-preset", "medium", "-x265-params", xp, "-f", "hevc",
                    out], check=True)
    size = os.path.getsize(out)
    subprocess.run(["ffmpeg", "-v", "error", "-y", "-i", out, "-f", "rawvideo", "-pix_fmt", "yuv420p10le", dec],
                   check=True)
    return size, dec


def ssim(a, b, peak=1023.0):
    c1, c2 = (0.01 * peak) ** 2, (0.03 * peak) ** 2
    mu_a = cv2.GaussianBlur(a, (11, 11), 1.5)
    mu_b = cv2.GaussianBlur(b, (11, 11), 1.5)
    saa = cv2.GaussianBlur(a * a, (11, 11), 1.5) - mu_a ** 2
    sbb = cv2.GaussianBlur(b * b, (11, 11), 1.5) - mu_b ** 2
    sab = cv2.GaussianBlur(a * b, (11, 11), 1.5) - mu_a * mu_b
    m = ((2 * mu_a * mu_b + c1) * (2 * sab + c2)) / ((mu_a ** 2 + mu_b ** 2 + c1) * (saa + sbb + c2))
    return float(m[8:-8, 8:-8].mean())


def metrics(ref_frames, test_frames, skip=12):
    mse, ss = [], []
    for r, t in zip(ref_frames[skip:], test_frames[skip:]):
        mse.append(np.mean((r - t) ** 2))
        ss.append(ssim(r, t))
    m = np.mean(mse)
    return (10 * np.log10(1023.0 ** 2 / m) if m > 0 else float("inf")), float(np.mean(ss))


def bd_rate(ref_pts, test_pts):
    """Average bitrate difference (%) of test vs ref at equal PSNR, over the overlapping
    PSNR range; piecewise-linear log-rate interpolation (monotone RD points)."""
    def curve(pts):
        pts = sorted(pts, key=lambda p: p[1])
        return np.array([p[1] for p in pts]), np.log(np.array([p[0] for p in pts], dtype=float))
    rp, rr = curve(ref_pts)
    tp, tr = curve(test_pts)
    lo, hi = max(rp.min(), tp.min()), min(rp.max(), tp.max())
    if hi - lo < 0.2:
        return float("nan"), lo, hi
    q = np.linspace(lo, hi, 50)
    return float(100 * (np.exp(np.mean(np.interp(q, tp, tr) - np.interp(q, rp, rr))) - 1)), lo, hi


CRFS = [14, 18, 22, 26, 30, 34]


def main():
    d = sys.argv[1]
    w, h = (int(sys.argv[2]), int(sys.argv[3])) if len(sys.argv) > 3 else (384, 256)
    methods = [l.rstrip("\n").split("\t") for l in open(os.path.join(d, "methods.txt"))]
    clean_yuv = os.path.join(d, "clean.yuv")
    write_yuv(clean_yuv, load_f32(os.path.join(d, "clean.f32"), w, h))
    ref = read_y(clean_yuv, w, h)
    secs = len(ref) / 30.0
    print(f"sequence {d}: {w}x{h}, {len(ref)} frames; libx265 Main10 HLG, CRF {CRFS}; quality = PSNR / SSIM of "
          f"10-bit HLG luma vs the CLEAN reference (frames 12+); rate in bit/pixel/frame")
    rows = [("clean (upper bound)", "-", clean_yuv)]
    for idx, name, s, reached in methods:
        yuv = os.path.join(d, f"m{idx}.yuv")
        if not os.path.exists(yuv):
            write_yuv(yuv, load_f32(os.path.join(d, f"m{idx}.f32"), w, h))
        rows.append((name, s + ("" if reached == "1" else "*"), yuv))
    curves = {}
    for name, s, yuv in rows:
        pre = metrics(ref, read_y(yuv, w, h))[0]
        pts = []
        for crf in CRFS:
            size, dec = encode_decode(yuv, w, h, yuv + f".crf{crf}", "crf", crf)
            p, ss = metrics(ref, read_y(dec, w, h))
            pts.append((8 * size / (w * h * len(ref)), p, ss))
        curves[name] = pts
        txt = "  ".join(f"{b:.3f}:{p:.2f}" for b, p, _ in pts)
        print(f"{name:30s} {s:>7s} | pre {pre:6.2f} dB | {txt}", flush=True)
    base = [(b, p) for b, p, _ in curves["none"]]
    print("\nBD-rate vs no denoise (negative = fewer bits for the same PSNR vs clean), and best PSNR reached:")
    for name, _, _ in rows:
        if name == "none":
            continue
        br, lo, hi = bd_rate(base, [(b, p) for b, p, _ in curves[name]])
        best = max(p for _, p, _ in curves[name])
        print(f"  {name:30s} BD-rate {br:+7.1f}%  (over {lo:.1f}-{hi:.1f} dB)   best {best:.2f} dB")


if __name__ == "__main__":
    main()

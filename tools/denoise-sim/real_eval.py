#!/usr/bin/env python3
"""Real-footage evaluation of the CPU denoise references (no clean reference exists).

    real_eval.py SIM_BINARY CROPS_DIR

CROPS_DIR holds <name>.f32 (float32 linear RGB nits, Rec.709, frame after frame) and
crops.json {name: [x, y, w, h]}. For each crop and method the sequence is processed with
`denoise_sim --process`, then:
  * temporal noise: per-pixel temporal std of F(Y) over frames 20+ (motion included),
    output / input, separately for flat and edge pixels (edge = top 20 % gradient of the
    temporal-mean input);
  * removed signal: rms of F(Y_in) - F(Y_out) on edge and on flat pixels (structure
    removal shows up as edge >> flat);
  * bits: libx265 CRF 18 size of the HLG-encoded output relative to the input.
Images of the removed signal are written next to the outputs for visual inspection;
they are not part of the repository (the footage shows real people).
"""
import json
import os
import subprocess
import sys

import cv2
import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import codec_test as ct  # noqa: E402

METHODS = [  # index in denoise_sim make_methods(), strength (matched on the encoded-noise model)
    (1, "HQ temporal", 6.28),
    (2, "HQ spatial+temporal", 4.0),
    (3, "NLM spatial 3x3/7x7", 1.5),
    (4, "TNLM A 3x3 s7 t7", 2.0),
    (6, "TNLM B 3x3 s7 t3 g9", 1.5),
    (7, "TNLM B 3x3 t1 only g9", 1.75),
]


def F(rgb):
    y = rgb @ np.array([0.2126, 0.7152, 0.0722])
    return np.sign(y) * np.log2(1 + np.abs(y) / 0.1)


def load(path, w, h):
    return np.fromfile(path, dtype=np.float32).reshape(-1, h, w, 3)


def crf_size(seq, w, h, tmp, crf=18):
    ct.write_yuv(tmp + ".yuv", seq.astype(np.float64))
    size, _ = ct.encode_decode(tmp + ".yuv", w, h, tmp, "crf", crf)
    for ext in (".yuv", ".hevc", ".dec.yuv"):
        if os.path.exists(tmp + ext):
            os.remove(tmp + ext)
    return size


def main():
    sim, d = sys.argv[1], sys.argv[2]
    crops = json.load(open(os.path.join(d, "crops.json")))
    only = sys.argv[3:] or list(crops)
    print("crop       method                  | tstd flat  tstd edge | removed flat  edge  edge/flat | CRF18 size")
    for name in only:
        x, y, w, h = crops[name]
        src = os.path.join(d, name + ".f32")
        inp = load(src, w, h)
        n = inp.shape[0]
        fin = F(inp.astype(np.float64))
        mean = fin[20:].mean(0)
        gx = cv2.Sobel(mean, cv2.CV_64F, 1, 0)
        gy = cv2.Sobel(mean, cv2.CV_64F, 0, 1)
        g = np.hypot(gx, gy)
        edge = g >= np.percentile(g, 80)
        flat = g <= np.percentile(g, 40)
        tin = fin[20:].std(0)
        base = crf_size(inp, w, h, os.path.join(d, name + "_in"))
        print(f"{name:10s} {'input':23s} | {np.median(tin[flat]):.4f}     {np.median(tin[edge]):.4f}  |"
              f"      -        -      -     | {base / 1024:7.1f} kB ({8 * base / (w * h * n):.3f} bpp)", flush=True)
        for idx, mname, S in METHODS:
            out_path = os.path.join(d, f"{name}_m{idx}.f32")
            if not os.path.exists(out_path):
                subprocess.run([sim, "--process", str(idx), str(S), src, out_path, str(w), str(h), str(n)],
                               check=True)
            out = load(out_path, w, h)
            fo = F(out.astype(np.float64))
            tout = fo[20:].std(0)
            rem = fin - fo
            rf = np.sqrt(np.mean(rem[20:, flat] ** 2))
            re_ = np.sqrt(np.mean(rem[20:, edge] ** 2))
            size = crf_size(out, w, h, os.path.join(d, f"{name}_m{idx}"))
            print(f"{name:10s} {mname:23s} | {np.median(tout[flat] / np.maximum(tin[flat], 1e-9)):.3f}x     "
                  f"{np.median(tout[edge] / np.maximum(tin[edge], 1e-9)):.3f}x  |"
                  f"  {rf:.4f}  {re_:.4f}  {re_ / max(rf, 1e-12):5.2f}  | {size / 1024:7.1f} kB "
                  f"({100 * (size / base - 1):+.1f}%)", flush=True)
            # removed-signal image (frame 70), gain 16, mid-grey = 0, for visual inspection only
            img = np.clip(0.5 + 16 * rem[70], 0, 1)
            cv2.imwrite(os.path.join(d, f"{name}_m{idx}_removed.png"), (img * 255).astype(np.uint8))


if __name__ == "__main__":
    main()

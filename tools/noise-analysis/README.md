# Noise analysis scripts (docs/denoise/NOISE_MODEL.md)

The scripts are run on a clip decoded outside the repository. Footage is never committed.

```sh
ffprobe -v error -select_streams v -show_entries frame=pkt_size,pict_type -of csv=p=0 CLIP > ftypes.csv
ffmpeg -i CLIP -vf "select='eq(pict_type\,I)'" -vsync 0 -f rawvideo -pix_fmt yuv420p10le iframes.yuv
python3 noise_I.py        # level table, chroma, autocorrelation, spectrum (I frames 1-8)
python3 row_banding.py    # row-structured share of the noise
ffmpeg -i CLIP -vf "select='between(n\,15\,134)'" -vsync 0 -f rawvideo -pix_fmt yuv420p10le seq120.yuv
python3 prep_crops.py     # linear-nits crops for tools/denoise-sim/real_eval.py (writes ../real/)
```

`clipio.py` assumes 4560 x 2160 yuv420p10le. Change `W` and `H` there for other canvases.

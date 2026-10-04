"""Syntax/type check of an OBS .effect file with Microsoft's DXC (HLSL).

OBS compiles .effect files only when it loads them, so CI cannot catch shader
errors. This converts the effect to plain HLSL (drops techniques, maps
texture2d / sampler_state / TARGET / POSITION) and compiles each pixel shader
entry point. It is a pre-flight check, not proof: OBS's own parser and fxc
(SM 4/5) can still differ, so the OBS log remains the final gate.

    python3 tools/check-effect-hlsl.py /path/to/dxc data/effects/foo.effect PSEntry [PSEntry ...]

DXC Linux build: github.com/microsoft/DirectXShaderCompiler/releases (set
LD_LIBRARY_PATH to its lib/ directory).
"""
import os, re, subprocess, sys, tempfile

dxc, effect, entries = sys.argv[1], sys.argv[2], sys.argv[3:]
s = open(effect).read()
s = re.sub(r"technique\s+\w+\s*\{.*?\n\}\n", "", s, flags=re.S)
s = re.sub(r"sampler_state\s+(\w+)\s*\{.*?\};", r"SamplerState \1;", s, flags=re.S)
s = re.sub(r"\btexture2d\b", "Texture2D", s)
s = re.sub(r":\s*TARGET\b", ": SV_Target", s)
s = re.sub(r":\s*POSITION\b", ": SV_Position", s)
with tempfile.NamedTemporaryFile("w", suffix=".hlsl", delete=False) as f:
    f.write(s)
fails = 0
for e in entries:
    r = subprocess.run([dxc, "-T", "ps_6_0", "-E", e, f.name, "-Fo", os.devnull], capture_output=True, text=True)
    msgs = [l for l in (r.stdout + r.stderr).splitlines()
            if ("error" in l or "warning" in l) and "Initializer of external global" not in l]
    print(f"{e}: {'OK' if r.returncode == 0 and not msgs else 'FAIL'}")
    for l in msgs:
        print("   ", l)
    fails += r.returncode != 0
sys.exit(1 if fails else 0)

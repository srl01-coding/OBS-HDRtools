# NVIDIA Optical Flow SDK interface headers (vendored)

- `nvOpticalFlowCommon.h`, `nvOpticalFlowCuda.h`: copied unmodified from
  https://github.com/NVIDIA/NVIDIAOpticalFlowSDK (BSD 3-clause licence, in each file's
  header; NV_OF_API_VERSION 2.0).
- `shim/cuda.h`: our own minimal declarations of the CUDA driver API types those headers
  reference (opaque handles only), so no CUDA toolkit is needed to build. The CUDA driver
  (`nvcuda.dll`) and the optical-flow runtime (`nvofapi64.dll`) ship with the NVIDIA
  display driver and are loaded at runtime with `LoadLibrary`; nothing NVIDIA is linked
  or redistributed.

Used only by `src/denoise/nvof-probe.cpp` (P2.5B capability probe). The D3D11 interface
header (`nvOpticalFlowD3D11.h`) is not in that repository; it comes with the full SDK
download (NVIDIA developer account, SDK licence) and is needed for P2.5B integration.

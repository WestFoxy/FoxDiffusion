# Static engine validation — 2026-11-06

## Environment

- Windows x64; NVIDIA GeForce RTX 4080 SUPER, 16 GB VRAM.
- NVIDIA driver 610.88; `cuDriverGetVersion` reports CUDA 13.3.
- Visual Studio 2026 installation, MSVC v143 19.44.35227, CMake 4.3, Ninja.
- No installed CUDA Toolkit/nvcc was found. Official NVIDIA CUDA 12.8 Update 1
  redistributables were unpacked into `build/toolchain/cuda`, with archive
  SHA-256 checked against `redistrib_12.8.1.json`. No system installation or
  driver change was performed.
- stable-diffusion.cpp `c678dfe704a2230342376b46add9c8ca736a653d`, GGML
  `f583f393cd5dfdc129360bbf75cb3d49ccc837a8`; both upstream worktrees clean.

## Build and imports

Release built in `build/static`, using `CMAKE_CUDA_ARCHITECTURES=89` for this
GPU validation. This particular binary targets Ada (SM 89, with PTX); for
distribution to older GPUs, omit that override and build the upstream portable
architecture set. `build/Release/` and the legacy source backup were not changed.

`ctest --test-dir build/static -C Release --output-on-failure`: **2/2 passed**
(`cuda-deferred-startup`, `img2img-processing`).

`dumpbin /dependents fox-diffusion.exe`:

- Ordinary imports: ole32, SHELL32, GDI32, OPENGL32, WINHTTP, bcrypt, KERNEL32,
  USER32, ADVAPI32, IMM32 (all Windows libraries).
- Delay imports: `cublas64_12.dll`, `cublasLt64_12.dll`, `nvcuda.dll`.
- No engine/GGML, cudart, OpenMP or Visual C++ runtime DLL import.
- WINHTTP is used only for downloading the NVIDIA archive over HTTPS; inference
  is a direct C API call and has no HTTP server/client connection.

## Portable application and GPU checks

1. Copied only `fox-diffusion.exe` (about 89 MB) into `build/portable-check`.
   Launched and visually inspected the UI before any CUDA DLL was present.
   The footer correctly displayed CUDA driver 13.3 / engine 12.8; the app
   created its own `config/` beside the EXE.
2. Selected local `DreamShaper_8_pruned.safetensors` and submitted a 512×512,
   20-step Euler/discrete txt2img request for a red ceramic teapot. The app
   downloaded the 563,660,944-byte NVIDIA cuBLAS archive, checked SHA-256,
   and extracted the two DLLs and license. Observed projected live preview,
   followed by a completed 512×512 image in the UI and PNG in `output/`.
3. Started a new runtime-check process in that directory. It verified and
   loaded the local DLLs without downloading again. Output:
   `PASS: CUDA runtime initialized on demand`.
4. Ran the embedded-backend smoke test with DreamShaper, 256×256 input and
   `4x-UltraSharp.pth`. All checks passed:
   - txt2img and an actual preview callback before completion;
   - img2img;
   - inpaint: protected pixels unchanged, masked pixels changed;
   - ESRGAN followed by multi-tile SD upscale to 384×384;
   - cancellation of a 100-step img2img request after two preview callbacks;
   - successful generation after cancellation.

Commands (substitute local model paths):

```powershell
./build/portable-check/cuda-runtime-smoke.exe prepare
./build/portable-check/img2img-smoke.exe "D:/Models/DreamShaper_8_pruned.safetensors" 256 "D:/Models/4x-UltraSharp.pth"
```

Local evidence retained in `build/gpu-smoke.log`, `build/static-imports.txt`,
`build/static-build-final.log`, and `build/portable-check/output/`.

## Limits

- No physical test on a machine without an NVIDIA driver or with an older
  driver; those paths are explicitly checked before GPU initialization/download.
- This run did not exercise Qwen, SDXL, LoRA, embeddings, TAE/VAE preview modes
  or other GPU architectures. Their request parameters remain wired to the C API.
- Cancellation is cooperative. Loading a model and an in-flight ESRGAN tile
  must return before resources can be freed; the UI thread does not wait for
  inference on an ordinary Cancel click. Closing the app waits for safe cleanup.
- A failed/interrupted download is retryable, but network-failure injection
  and disk-full recovery were not exercised in this run.

The original cancellation test waited for the requested number of sampling
steps, which was wrong for img2img strength. It was corrected to wait for two
actual preview callbacks, and the complete GPU smoke suite was rerun successfully.

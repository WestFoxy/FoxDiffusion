# Native img2img

**File > Settings** opens a modal with a blurred backdrop and Output, Interface,
Preview and Colors tabs. The backdrop uses a mild blur without a light modal
overlay, and all UI rounding is disabled. Colors supports live editing, search,
and reset; color overrides persist by ImGui color name in settings.ini. Output has
independent automatic saving switches for txt2img and img2img/inpaint/scripts;
img2img automatic saving defaults to off. Manual Save always remains available.
Switches apply to results published after the change; queued saves finish.
With saving disabled only the current result stays available in memory.

Qwen-Image-2.1 GGUF diffusion weights are listed in Models. In Models > Loading > Qwen components,
choose Auto detection or explicit Qwen-Image-2.1 and configure its own 2.1 VAE,
Qwen3-VL-8B encoder and, for GGUF image editing, its vision projector. See the
[upstream setup guide](https://github.com/leejet/stable-diffusion.cpp/blob/master/docs/qwen_image_2.1.md)
for compatible component files. CPU offloading is enabled by default. Image
editing uses reference images and instruction prompts; local masks composite
the result. This is not SD's latent denoising/inpainting workflow. Generation
sizes are aligned to 32 pixels. The installed CUDA DLL already contains Qwen
Image 2.1 support. These changes wire that support through the UI and server.

Select **img2img** in the top bar. **Generate img2img** is at the top of the
Generation panel; sampler, scheduler, CFG, steps, seed and model are shared with
txt2img. All code is C++20, including the editor and script workflows.

Txt2img and img2img keep independent docking trees and window identities in
layout.ini. Switching modes keeps the hidden docking tree alive; shutdown saves
both layouts. **View > Reset current mode layout** resets only the active mode.
The new layout version starts from fresh defaults once; custom arrangements
made in this version persist independently for each mode.

Generation keeps the selected model and Generate/Cancel above its scrollable
controls. Prompt, Steps, CFG and size are visible; negative prompt, sampling/seed
and prompt tools are expandable. Models separates **Select model**, **Loading**
and **Extras**. Selection has a cached search list and disk sizes; Refresh keeps
the selected model when it still exists. Qwen components are in Loading;
embeddings, LoRA and metadata are in Extras. **Result > Apply to editor** sends
the result to img2img without changing prompt or sampling parameters.

The compact toolbar keeps file actions and transforms in **Image**, and brush,
selection, color, opacity and hardness in the current tool menu. Ctrl+O opens
an image and Ctrl+V pastes. Size/resize and batch/output settings are collapsible.

In the result viewer, wheel zooms at the pointer and left/middle drag pans.
**Fit** or double-click resets the view; **1:1** displays image pixels at native
size. **Enlarge** opens an inspection window (Esc closes it). **Use result**
updates the editor and clears the mask, preserving its view for equal-size
images; Undo restores the previous image and mask. Export is under **More**.
The standard ImGui font is used. **View > Reset workspace layout** restores
the default arrangement with a larger result pane; existing custom layouts
are otherwise preserved.

Large-image viewing keeps the full-resolution texture on the GPU and uses
generated mip levels only for minification. Native-scale and magnified views
sample the original level; editing and exports retain the original pixels.
Backend status is polled once per frame, with image data copied only when a
new image revision is published. Panning and zooming do not copy or re-upload
the image. Active interaction is paced by VSync; idle redraws target 30 FPS.
Model disk sizes are cached on Refresh and shown in a separate column.
The model list clips offscreen rows and caches tooltip paths; tooltips appear
after a short hover delay. CPU/GPU telemetry is sampled on a background thread
so NVIDIA driver calls cannot block the UI render loop.

## Editor

- Open PNG/JPEG/BMP/TGA, drop an image onto the main window, paste a Windows
  bitmap, create a blank canvas, or send a generated result to img2img.
- Brush size is measured in source pixels. Opacity and hardness control strokes.
- Eyedropper samples the actual image pixels, without the mask preview tint.
  Use **Pick from image** in the tool menu or **I**, then click the canvas.
  After picking, the tool returns to Brush automatically. **B** selects Brush;
  **E** selects Eraser. Alt+click picks a color while using Brush/Eraser.
  Alt no longer activates ImGui menu navigation. The toolbar also has a color
  swatch after the wider, logarithmic size slider. Ctrl+click size to type it.
- **[ / ]** changes brush size with increasing steps: 1 px up to 10,
  then 2, 4, 10, 20 and 50 px. Holding a key repeats the adjustment.
- Inpaint paints a mask; Inpaint sketch paints both color and mask. Erasing
  a sketch restores source pixels. Uploaded masks use white for regenerated areas.
  Painted masks use full opacity independently of color opacity. Mask blur
  feathers outward and preserves the selected interior, avoiding a translucent
  copy of the source being blended back over the generated subject. Soft brush
  edges and gray values in uploaded masks still provide partial coverage.
- Rectangle, ellipse and lasso add selections. Hold Alt when finishing a shape
  to subtract it. Shapes can also define a crop in ordinary img2img.
- Wheel zooms around the pointer; middle drag or Space + drag pans; Fit resets
  the view. Undo/Redo also cover mask changes, flips, rotation and crops.
- Ctrl+Z / Ctrl+Shift+Z / Ctrl+Y work when the canvas has focus.
- Masks can be loaded and exported separately. Dimensions must match the image.
- Brush, eraser and selection history stores only touched 128-pixel tiles.
  Undo/Redo share a limit of 256 steps and approximately 512 MiB of pixel payload;
  whole-image operations still use snapshots. Larger edits consume more history.
  Painting uploads only the changed rectangle and rebuilds viewing mip levels
  at stroke end, preserving full-resolution image and mask pixels.
  Fixed 16/64-megapixel limits are removed. Large operations check available RAM;
  the current image codec still requires an RGBA buffer below 2 GiB. Image views
  use GPU texture tiles with gutters, preserving native resolution even when
  the full image exceeds the GPU's maximum single-texture dimensions.

Final img2img results are published before automatic PNG/metadata saving, which
runs on a separate worker with a bounded queue. **Use result** does not wait for
the file. The footer reports pending saves and errors; gallery entries appear
only after successful writes. Closing the app drains pending saves.

## Generation

Denoising strength ranges from 0 to 1. Zero bypasses inference and returns the
prepared source. Width and height are aligned to 8 pixels. Scale by derives
the target size from the source. Resize modes stretch, crop to fill, or fit and
extend edge pixels.

Inpaint supports mask inversion, blur, original/color-fill/pixel-noise/neutral
content, whole-image processing and only-masked processing with padding. With
only-masked processing the output retains source dimensions, and generated
pixels are composited with the original-resolution mask. Blur intentionally
extends the transition beyond a hard selection; zero blur preserves every
pixel outside the mask exactly.

Batch count and size run sequentially to limit VRAM. Batch mode reads images
from a directory, optionally matches masks by filename stem, and optionally
reads `.txt` prompts or embedded PNG parameters. Missing or mismatched masks
fail with a message rather than silently applying an unrelated mask.

Generation runs from a snapshot of settings and image data. Subsequent editor
changes do not alter queued work. Cancel stops the current workflow and its
owned inference process. Already completed images remain saved.

## Native scripts

| Script | Behavior |
| --- | --- |
| SD upscale | Bilinear or selected ESRGAN model, then overlapping diffusion tiles and weighted seam blending. Width/height define diffusion tile dimensions. |
| Loopback | Feeds each output into the next iteration, ramps denoising linearly, increments seeds, saves every iteration. |
| Outpainting | Independent left/top/right/bottom extension, edge initialization and masked generation with source compositing. |
| X/Y/Z plot | Denoising, CFG, steps and seed axes; comma-separated numeric values; same starting seed for comparisons; labelled grid for each Z slice. |
| Prompts from textbox / file | One complete positive prompt per line. |
| Prompt matrix | `base | optional part | another optional part`, all subsets, same seed for comparison. |

Images are saved beside the executable under `output`, unless an output
directory is selected. Each result includes embedded PNG parameters and a
matching `.txt`. Select a previous result with the Result slider, then export
or send it back to the editor. Imported PNG parameters are applied only after
clicking **Use PNG generation parameters**.

ESRGAN calls the installed native CUDA library directly, loads its model once,
and processes source regions with 32-pixel context halos. Each region is
resampled to the requested output scale before being assembled. A 4x model
used for a 2x request never creates a full 4x canvas or intermediate PNG.
CUDA inference within each region uses 128-pixel tiles. The live preview shows
the progressively updated canvas, highlights the last completed region, and
reports completed/total regions and target dimensions. This working preview
is at most 1536 pixels on its long side; the saved final result is full-size.
ESRGAN and diffusion have separate tile progress. Cancel is checked between
ESRGAN regions; it does not interrupt an in-flight CUDA kernel.

## Compatibility boundaries

This is a native implementation of the workflows above, **not complete
AUTOMATIC1111 parity**. In particular:

- Python scripts/extensions, custom Python code, CLIP/DeepBooru interrogation,
  and the alternative img2img noise-inversion script are not implemented.
- Pixel noise and neutral pixels are explicitly named as such. They are not
  A1111's latent-noise / latent-nothing modes. Latent resize is not exposed.
- Outpainting uses the native algorithm above; it is not a bit-for-bit port of
  either A1111 outpainting script. Prompt matrix does not generate its own grid.
- XYZ exposes four numeric axes, not every A1111 axis. Checkpoint/style axes,
  full extension hooks and every advanced seed/sampler option are not present.
- No TIFF/WebP import, EXIF auto-rotation, compressed PNG metadata or arbitrary
  A1111 override dictionaries. Uncompressed `tEXt`/`iTXt` parameters are supported.
- Mask blur is a three-pass separable Gaussian approximation. Engine sampling
  differs from A1111; identical prompts/seeds do not promise identical pixels.
- Image CFG requires a compatible checkpoint. ESRGAN architectures must be
  supported by the installed stable-diffusion.cpp build.

## Verified on this machine

- Release build with Visual Studio 2026, C++20, no Python runtime.
- C++ tests for raster geometry, blur, crop, protected pixels, resize, zero
  denoising, tile coverage and blending, loopback, plot combinations/grids,
  prompt lists/matrix, outpaint, invalid inputs, cancellation, Unicode file
  paths, PNG roundtrips, parameter import and batch directories.
- DreamShaper 8 (SD 1.5): real img2img, inpaint and multi-tile upscale.
- Juggernaut XL v9: real 1024x1024 img2img/inpaint and 1536x1536 tiled upscale.
- Juggernaut XL v9 plus 4x-UltraSharp: neural upscale followed by diffusion tiles.

These checks cover the listed scenarios; they are not a guarantee that every
checkpoint, driver, image format or possible workflow is free of defects.

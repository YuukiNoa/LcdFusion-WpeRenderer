# LcdFusion-WpeRenderer

Standalone **x64 headless Vulkan offscreen Scene renderer** (`wpe-renderer.exe`) for [LcdFusion](https://github.com/YuukiNoa/LcdFusion), licensed under **GPL-2.0** (matching upstream `catsout/wallpaper-scene-renderer`).

## Architecture & Isolation

- **Process & License Isolation**: Runs as an independent x64 child process communicating with `LcdFusion.exe` (x64, MIT) strictly over the public IPC v1 protocol. Never commit or bundle this GPL-2.0 binary into the MIT `LcdFusion` repository or portable zip.
- **Privilege Isolation**: When `LcdFusion.exe` runs elevated under UAC (High Integrity), it launches `wpe-renderer.exe --ipc-pipe <base>` using `explorer.exe`'s authentic Medium Integrity desktop token (`CreateProcessWithTokenW` + `CreateEnvironmentBlock`) over scoped-DACL unidirectional named pipes (`\\.\pipe\<base>_in` / `_out`), and stamps a Medium Mandatory Label (`S:(ML;;NW;;;ME)`) onto both the pipes and shared memory mapping. `ShmWriter` requests minimum required access (`FILE_MAP_READ | FILE_MAP_WRITE`). Standard `--ipc` (`stdin`/`stdout`) remains supported for non-elevated runs and fallback.
- **Headless Vulkan 1.1+ Offscreen Rendering**: Renders Wallpaper Engine Scene wallpapers (including embedded MP4 video textures and `fill` / `fit` / `stretch` FitMode) directly into `VK_FORMAT_B8G8R8A8_UNORM` offscreen images and copies frames via persistently-mapped `HOST_VISIBLE | HOST_COHERENT` staging buffers into double-buffered Windows shared memory (`Local\LcdFusion_Wpe_*`). Automatically scales non-screen-bound 4K/8K internal effect render targets and embedded video textures to match the offscreen output pixel density for low iGPU overhead on small LCDs.
- **Native Video Wallpapers**: Decodes Wallpaper Engine `video` wallpapers natively via Windows Media Foundation (`IMFSourceReader`, x64, optional DXGI/D3D11 hardware acceleration) with Video Processor pre-downscale for 2K/4K sources, zero-copy catch-up frame skipping for high-FPS videos, and cached bilinear LUTs into the same shared-memory IPC path.
- **Web Wallpapers (System WebView2 Offscreen)**: Renders Wallpaper Engine `web` wallpapers (`index.html`, WebGL, Spine, Live2D, Canvas, CSS) via Windows built-in Microsoft Edge WebView2 Runtime (dynamically loading `WebView2Loader.dll` or `EmbeddedBrowserWebView.dll` with zero extra deployed DLLs), mapping the wallpaper directory to `https://wallpaper.local/`, capturing GPU-composited frames via `targetFps`-paced CDP `Page.startScreencast`, and bridging `window.wallpaperPropertyListener` + `window.wallpaperRegisterAudioListener` + hardware `sensors` updates.
- **Multi-GPU Selection**: Supports explicit GPU selection across all three backends (`--list-gpus`, `--gpu`, and IPC `load.gpu`): Vulkan physical-device matching, Chromium/WebView2 `--use-adapter-luid`, and Media Foundation `IMFDXGIDeviceManager` bound to the chosen DXGI adapter LUID.
- **Audio Visualization (WASAPI Loopback + FFT)**: Captures system loopback audio via Windows WASAPI and computes 1024-point log-scale FFT spectrum uniforms (`g_AudioSpectrum{16,32,64}{Left,Right}`) and SceneScript `engine.registerAudioBuffers(16/32/64)`.
- **SceneScript JS Engine (QuickJS-ng, MIT) & Built-in Text Layers (GDI+)**: Embeds [`quickjs-ng/quickjs`](https://github.com/quickjs-ng/quickjs) statically (64 MB cap, per-call interrupt timeout) for ES2023/ESM Wallpaper Engine SceneScript (`WEColor`, `WEMath`, `WEVector`, `Vec2`/`Vec3`, `createScriptProperties`, `engine`, `thisLayer`/`thisScene`, `init`/`update`/`applyUserProperties`), plus Windows GDI+ `text` layer compositing with embedded VFS `.ttf`/`.otf` fonts.
- **No Bundled Assets**: Never packages or distributes Wallpaper Engine `assets/` or Workshop wallpapers; reads user-owned Steam directories at runtime.

## Upstream Source & Patch

Built on top of [`catsout/wallpaper-scene-renderer`](https://github.com/catsout/wallpaper-scene-renderer) at commit `8dde7e7` (with `third_party/SPIRV-Reflect`), [`quickjs-ng/quickjs`](https://github.com/quickjs-ng/quickjs) (MIT) at commit `dce8c0b`, plus [`patches/0001-windows-headless-vulkan-offscreen.patch`](patches/0001-windows-headless-vulkan-offscreen.patch).

To prepare a fresh checkout:

```powershell
./scripts/setup-upstream.ps1
```

## Building (MSYS2 UCRT64)

Prerequisites in `C:\msys64\ucrt64`: GCC/C++20, CMake, Ninja, Vulkan Headers/Loader, `glslang`, `SPIRV-Tools`, `Eigen3`, `nlohmann-json`, `liblz4`, `stb`, `webview2-loader` (headers only).

```powershell
$env:PATH = "C:\msys64\ucrt64\bin;" + $env:PATH
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j 8
```

This produces a standalone `build\wpe-renderer.exe` with no MSYS2 runtime DLL dependencies (depends only on Windows in-box DLLs: `KERNEL32.dll`, `USER32.dll`, `ADVAPI32.dll`, `ole32.dll`, `SHLWAPI.dll`, `dwmapi.dll`, `gdiplus.dll`, `MFPlat.DLL`, `MFReadWrite.dll`, `vulkan-1.dll`, and `api-ms-win-crt-*`; `web` wallpapers use the system Edge WebView2 Runtime).

## Installation into LcdFusion

Copy `build\wpe-renderer.exe` to `<LcdFusionDir>\renderer\wpe-renderer.exe` (or pick the executable manually inside LcdFusion's Wallpaper dialog).

## Standalone GPL-2.0 Packaging

```powershell
./scripts/pack-renderer.ps1 -Version 0.1.0
```

Produces `LcdFusion-WpeRenderer-<Version>-win-x64.zip` containing `renderer\wpe-renderer.exe`, `LICENSE.txt` (GPL-2.0), `README.md`, and `patches\`.

## CLI Verification Mode

```powershell
./build/wpe-renderer.exe `
  --dir "C:/Program Files (x86)/Steam/steamapps/workshop/content/431960/<id>" `
  --assets "C:/Program Files (x86)/Steam/steamapps/common/wallpaper_engine/assets" `
  --width 480 --height 480 --fit fill --frames 60 --dump test.png
```

Additional optional CLI flags: `--gpu <auto|pci:VVVV:DDDD:ORDINAL|name>`, `--list-gpus`, `--props '<json>'`, `--sensors '<json>'`, `--test-audio`.

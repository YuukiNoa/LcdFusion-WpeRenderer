# LcdFusion-WpeRenderer

Standalone **x64 headless Vulkan offscreen Scene renderer** (`wpe-renderer.exe`) for [LcdFusion](../LcdFusion), licensed under **GPL-2.0** (matching upstream `catsout/wallpaper-scene-renderer`).

## Architecture & Isolation

- **Process & License Isolation**: Runs as an independent x64 child process communicating with `LcdFusion.exe` (x86, MIT) strictly over the public IPC protocol (`LcdFusion/docs/wpe-renderer-ipc.md`). Never commit or bundle this GPL-2.0 binary into the MIT `LcdFusion` repository or portable zip.
- **Privilege Isolation**: When `LcdFusion.exe` runs elevated under UAC (High Integrity), it launches `wpe-renderer.exe --ipc-pipe <base>` using `explorer.exe`'s authentic Medium Integrity desktop token (`CreateProcessWithTokenW` + `CreateEnvironmentBlock`) over scoped-DACL unidirectional named pipes (`\\.\pipe\<base>_in` / `_out`), and stamps a Medium Mandatory Label (`S:(ML;;NW;;;ME)`) onto both the pipes and shared memory mapping. `ShmWriter` requests minimum required access (`FILE_MAP_READ | FILE_MAP_WRITE`). Standard `--ipc` (`stdin`/`stdout`) remains supported for non-elevated runs and fallback.
- **Headless Vulkan 1.1+ Offscreen Rendering**: Renders Wallpaper Engine Scene wallpapers directly into `VK_FORMAT_B8G8R8A8_UNORM` offscreen images and copies frames via persistently-mapped `HOST_VISIBLE | HOST_COHERENT` staging buffers into double-buffered Windows shared memory (`Local\LcdFusion_Wpe_*`).
- **Native Video Wallpapers**: Decodes Wallpaper Engine `video` wallpapers natively via Windows Media Foundation (`IMFSourceReader`, x64) into the same shared-memory IPC path.
- **Audio Visualization (WASAPI Loopback + FFT)**: Captures system loopback audio via Windows WASAPI and computes 256-point FFT spectrum uniforms (`g_AudioSpectrum{16,32,64}{Left,Right}`) and SceneScript `engine.registerAudioBuffers(16/32/64)`.
- **SceneScript JS Engine (QuickJS-ng, MIT) & Built-in Text Layers (GDI+)**: Embeds [`quickjs-ng/quickjs`](https://github.com/quickjs-ng/quickjs) statically (64 MB cap, per-call interrupt timeout) for ES2023/ESM Wallpaper Engine SceneScript (`WEColor`, `WEMath`, `WEVector`, `Vec2`/`Vec3`, `createScriptProperties`, `engine`, `thisLayer`/`thisScene`, `init`/`update`/`applyUserProperties`), plus Windows GDI+ `text` layer compositing with embedded VFS `.ttf`/`.otf` fonts.
- **No Bundled Assets**: Never packages or distributes Wallpaper Engine `assets/` or Workshop wallpapers; reads user-owned Steam directories at runtime.

## Upstream Source & Patch

Built on top of [`catsout/wallpaper-scene-renderer`](https://github.com/catsout/wallpaper-scene-renderer) at commit `8dde7e7` (with `third_party/SPIRV-Reflect`), [`quickjs-ng/quickjs`](https://github.com/quickjs-ng/quickjs) (MIT) at commit `dce8c0b`, plus [`patches/0001-windows-headless-vulkan-offscreen.patch`](patches/0001-windows-headless-vulkan-offscreen.patch).

To prepare a fresh checkout:

```powershell
./scripts/setup-upstream.ps1
```

## Building (MSYS2 UCRT64)

Prerequisites in `C:\msys64\ucrt64`: GCC/C++20, CMake, Ninja, Vulkan Headers/Loader, `glslang`, `SPIRV-Tools`, `Eigen3`, `nlohmann-json`, `liblz4`, `stb`.

```powershell
$env:PATH = "C:\msys64\ucrt64\bin;" + $env:PATH
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j 8
```

This produces a standalone `build\wpe-renderer.exe` with no MSYS2 runtime DLL dependencies (depends only on Windows in-box DLLs: `KERNEL32.dll`, `api-ms-win-crt-*`, `ole32.dll`, `gdiplus.dll`, `MFPlat.DLL`, `MFReadWrite.dll`, and `vulkan-1.dll`).

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
  --width 480 --height 480 --frames 60 --dump test.png
```

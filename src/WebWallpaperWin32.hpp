#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace lcdfusion_wpe
{

struct AudioSpectrumData;

// Offscreen Web wallpaper renderer powered by Windows built-in Microsoft Edge WebView2 Runtime.
// Dynamically loads WebView2Loader.dll or EmbeddedBrowserWebView.dll (zero extra deployed DLLs),
// maps the wallpaper folder to https://wallpaper.local/ (no CORS errors), bridges Wallpaper Engine's
// window.wallpaperPropertyListener and window.wallpaperRegisterAudioListener APIs, and captures
// GPU-composited frames (including WebGL/Spine/Live2D/Canvas) via CDP Page.startScreencast.
class WebWallpaperWin32 {
public:
    using FrameConsumer =
        std::function<void(const uint8_t* bgra, uint32_t width, uint32_t height, uint32_t stride)>;

    WebWallpaperWin32();
    ~WebWallpaperWin32();

    WebWallpaperWin32(const WebWallpaperWin32&)            = delete;
    WebWallpaperWin32& operator=(const WebWallpaperWin32&) = delete;

    void SetFrameConsumer(FrameConsumer consumer);
    // 0 = Fill (native viewport), 1 = Fit (16:9 letterbox), 2 = Stretch (16:9 stretched to output)
    void SetFitMode(int fitMode);

    bool Open(const std::string& wallpaperDirUtf8, const std::string& entryFileUtf8,
              uint32_t width, uint32_t height, const std::string& initialPropsJson,
              std::string& outError, std::string& outCode);
    bool Resize(uint32_t width, uint32_t height);
    void SetProperties(const std::string& changedPropsJson);
    void SetSensors(const std::string& sensorsJson);
    void SetPaused(bool paused);
    void FeedAudioSpectrum(const AudioSpectrumData& audio);
    bool RenderFrame(double deltaSec);
    void Close();

    bool     IsOpen() const;
    uint32_t Width() const;
    uint32_t Height() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace lcdfusion_wpe

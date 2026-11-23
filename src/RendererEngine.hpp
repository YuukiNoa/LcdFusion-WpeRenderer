#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace wallpaper
{
class Scene;
class WPSceneParser;
namespace audio
{
class SoundManager;
}
namespace rg
{
class RenderGraph;
}
namespace vulkan
{
class VulkanRender;
}
} // namespace wallpaper

namespace lcdfusion_wpe
{

class VideoPlayerWin32;
class WebWallpaperWin32;
class WasapiAudioAnalyzer;
class TextLayerRenderer;
class SceneScriptEngine;

struct LoadRequest {
    std::string dir;
    std::string assets;
    std::string shmName;
    std::string initialPropsJson;
    std::string fitMode { "fill" };
    uint32_t    width { 480 };
    uint32_t    height { 480 };
    uint32_t    fps { 30 };
};

class RendererEngine {
public:
    using FrameConsumer =
        std::function<void(const uint8_t* bgra, uint32_t width, uint32_t height, uint32_t stride)>;

    RendererEngine();
    ~RendererEngine();

    RendererEngine(const RendererEngine&)            = delete;
    RendererEngine& operator=(const RendererEngine&) = delete;

    void SetFrameConsumer(FrameConsumer consumer);

    bool Load(const LoadRequest& req, std::string& outError, std::string& outCode);
    bool Resize(uint32_t width, uint32_t height, std::string& outError);
    void SetFitMode(const std::string& fitMode);
    bool SetProperties(const std::string& propsJson, std::string& outError);
    void SetSensors(const std::string& sensorsJson);
    void SetPaused(bool paused);
    bool RenderFrame(double deltaSec);
    void Shutdown();

    // Deterministic PCM injection for offline audio-visualization verification.
    void InjectTestAudioTone(float frequencyHz, float amplitude);

    bool        IsLoaded() const { return m_loaded; }
    bool        IsVideo() const { return m_isVideo; }
    bool        IsWeb() const { return m_isWeb; }
    std::string BackendName() const {
        if (m_isWeb) return "webview2-x64";
        if (m_isVideo) return "mf-video-x64";
        return "vulkan-x64";
    }
    uint32_t    Width() const { return m_width; }
    uint32_t    Height() const { return m_height; }

private:
    struct SceneVideoTexture {
        std::string                       texKey;
        std::wstring                      tempFile;
        std::unique_ptr<VideoPlayerWin32> player;
        std::vector<uint8_t>              lastRgba;
        uint32_t                          width { 0 };
        uint32_t                          height { 0 };
        bool                              dirty { false };
    };

    bool InitVulkanIfNeeded(uint32_t width, uint32_t height, std::string& outError);
    bool RebuildSceneFromCachedSource(std::string& outError);
    void ClearSceneVideoTextures();
    void HandleVulkanFrame(const uint8_t* bgra, uint32_t width, uint32_t height, uint32_t stride);

    std::unique_ptr<wallpaper::WPSceneParser>        m_parser;
    std::unique_ptr<wallpaper::audio::SoundManager>  m_sound;
    std::unique_ptr<wallpaper::vulkan::VulkanRender> m_render;
    std::shared_ptr<wallpaper::Scene>                m_scene;
    std::unique_ptr<wallpaper::rg::RenderGraph>      m_rg;

    std::unique_ptr<VideoPlayerWin32>    m_video;
    std::unique_ptr<WebWallpaperWin32>   m_web;
    std::unique_ptr<WasapiAudioAnalyzer> m_audio;
    std::unique_ptr<TextLayerRenderer>   m_text;
    std::unique_ptr<SceneScriptEngine>   m_scripts;
    std::vector<SceneVideoTexture>       m_sceneVideoTextures;
    std::vector<uint8_t>                 m_compositeBgra;

    LoadRequest m_lastReq {};
    std::string m_sceneId;
    std::string m_rawSceneSrc;
    std::string m_lastEffectiveSceneSrc;
    std::string m_pkgPathUtf8;
    std::string m_userPropsJson { "{}" };
    std::string m_lastSensorsJson;
    int         m_fitMode { 0 }; // 0 = fill, 1 = fit, 2 = stretch

    FrameConsumer m_frameConsumer;
    bool          m_loaded { false };
    bool          m_isVideo { false };
    bool          m_isWeb { false };
    bool          m_paused { false };
    uint32_t      m_width { 0 };
    uint32_t      m_height { 0 };
};

} // namespace lcdfusion_wpe

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
class WasapiAudioAnalyzer;

struct LoadRequest {
    std::string dir;
    std::string assets;
    std::string shmName;
    std::string initialPropsJson;
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
    bool SetProperties(const std::string& propsJson, std::string& outError);
    void SetPaused(bool paused);
    bool RenderFrame(double deltaSec);
    void Shutdown();

    // Deterministic PCM injection for offline audio-visualization verification.
    void InjectTestAudioTone(float frequencyHz, float amplitude);

    bool        IsLoaded() const { return m_loaded; }
    bool        IsVideo() const { return m_isVideo; }
    std::string BackendName() const { return m_isVideo ? "mf-video-x64" : "vulkan-x64"; }
    uint32_t    Width() const { return m_width; }
    uint32_t    Height() const { return m_height; }

private:
    bool InitVulkanIfNeeded(uint32_t width, uint32_t height, std::string& outError);
    bool RebuildSceneFromCachedSource(std::string& outError);

    std::unique_ptr<wallpaper::WPSceneParser>        m_parser;
    std::unique_ptr<wallpaper::audio::SoundManager>  m_sound;
    std::unique_ptr<wallpaper::vulkan::VulkanRender> m_render;
    std::shared_ptr<wallpaper::Scene>                m_scene;
    std::unique_ptr<wallpaper::rg::RenderGraph>      m_rg;

    std::unique_ptr<VideoPlayerWin32>    m_video;
    std::unique_ptr<WasapiAudioAnalyzer> m_audio;

    LoadRequest m_lastReq {};
    std::string m_sceneId;
    std::string m_rawSceneSrc;
    std::string m_pkgPathUtf8;
    std::string m_userPropsJson { "{}" };

    FrameConsumer m_frameConsumer;
    bool          m_loaded { false };
    bool          m_isVideo { false };
    bool          m_paused { false };
    uint32_t      m_width { 0 };
    uint32_t      m_height { 0 };
};

} // namespace lcdfusion_wpe

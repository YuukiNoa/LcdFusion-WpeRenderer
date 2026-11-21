#include "RendererEngine.hpp"
#include "AudioSpectrumWin32.hpp"
#include "VideoPlayerWin32.hpp"

#include "Audio/SoundManager.h"
#include "Fs/CBinaryStream.h"
#include "Fs/PhysicalFs.h"
#include "Fs/VFS.h"
#include "Interface/IShaderValueUpdater.h"
#include "Particle/ParticleSystem.h"
#include "RenderGraph/RenderGraph.hpp"
#include "Scene/Scene.h"
#include "SceneWallpaperSurface.hpp"
#include "Utils/Logging.h"
#include "VulkanRender/SceneToRenderGraph.hpp"
#include "VulkanRender/VulkanRender.hpp"
#include "WPPkgFs.hpp"
#include "WPSceneParser.hpp"
#include "WPShaderValueUpdater.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <sstream>
#include <unordered_map>

namespace lcdfusion_wpe
{

namespace
{

std::string ToLowerAscii(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string ReadFileUtf8(const std::filesystem::path& path) {
    auto u8 = path.u8string();
    std::string utf8(reinterpret_cast<const char*>(u8.data()), u8.size());
    auto stream = wallpaper::fs::CreateCBinaryStream(utf8);
    if (! stream) return {};
    return stream->ReadAllStr();
}

std::string JsonScalarToString(const nlohmann::json& j) {
    if (j.is_string()) return j.get<std::string>();
    if (j.is_boolean()) return j.get<bool>() ? "true" : "false";
    if (j.is_number_integer()) return std::to_string(j.get<int64_t>());
    if (j.is_number_unsigned()) return std::to_string(j.get<uint64_t>());
    if (j.is_number_float()) {
        std::ostringstream oss;
        oss << j.get<double>();
        return oss.str();
    }
    return j.dump();
}

bool JsonToBool(const nlohmann::json& j, bool fallback) {
    if (j.is_boolean()) return j.get<bool>();
    if (j.is_number()) return j.get<double>() != 0.0;
    if (j.is_string()) {
        std::string s = ToLowerAscii(j.get<std::string>());
        if (s == "true" || s == "1" || s == "yes" || s == "on") return true;
        if (s == "false" || s == "0" || s == "no" || s == "off") return false;
    }
    return fallback;
}

bool TryJsonToDouble(const nlohmann::json& j, double& out) {
    if (j.is_number()) {
        out = j.get<double>();
        return true;
    }
    if (j.is_string()) {
        try {
            out = std::stod(j.get<std::string>());
            return true;
        } catch (...) {
            return false;
        }
    }
    if (j.is_boolean()) {
        out = j.get<bool>() ? 1.0 : 0.0;
        return true;
    }
    return false;
}

// Recursively substitutes Wallpaper Engine `"user": "<prop>"` / `"user": {"name": "...", "condition": "..."}`
// bindings inside scene.json before passing it to WPSceneParser (case-insensitive property key lookup).
void ApplyUserPropertiesRecursive(
    nlohmann::json&                                      node,
    const std::unordered_map<std::string, nlohmann::json>& userPropsLower) {
    if (node.is_array()) {
        for (auto& el : node) ApplyUserPropertiesRecursive(el, userPropsLower);
        return;
    }
    if (! node.is_object()) return;

    auto applyTypedValue = [](nlohmann::json& dest, const nlohmann::json& ov) {
        if (dest.is_boolean()) {
            dest = JsonToBool(ov, dest.get<bool>());
        } else if (dest.is_number()) {
            double d = 0.0;
            if (TryJsonToDouble(ov, d)) dest = d;
        } else if (dest.is_string()) {
            std::string orig = dest.get<std::string>();
            if (ov.is_number()) {
                // If original string is a multi-component vector like "1 1" or "1 1 1" and override is a scalar slider,
                // replicate the scalar across components so WPJson's array parser succeeds.
                std::istringstream iss(orig);
                std::string        tok;
                int                compCount = 0;
                bool               allNums   = true;
                while (iss >> tok) {
                    ++compCount;
                    try {
                        std::size_t idx = 0;
                        (void)std::stod(tok, &idx);
                        if (idx != tok.size()) allNums = false;
                    } catch (...) {
                        allNums = false;
                    }
                }
                if (allNums && compCount >= 2 && compCount <= 4) {
                    std::string s = JsonScalarToString(ov);
                    std::string joined;
                    for (int i = 0; i < compCount; ++i) {
                        if (i > 0) joined.push_back(' ');
                        joined += s;
                    }
                    dest = joined;
                    return;
                }
            }
            dest = JsonScalarToString(ov);
        } else {
            dest = ov;
        }
    };

    if (node.contains("user")) {
        const auto& u = node["user"];
        if (u.is_string()) {
            auto it = userPropsLower.find(ToLowerAscii(u.get<std::string>()));
            if (it != userPropsLower.end()) {
                if (node.contains("value")) {
                    applyTypedValue(node["value"], it->second);
                } else {
                    node["value"] = it->second;
                }
            }
        } else if (u.is_object() && u.contains("name") && u["name"].is_string()) {
            auto it = userPropsLower.find(ToLowerAscii(u["name"].get<std::string>()));
            if (it != userPropsLower.end()) {
                const auto& ov = it->second;
                if (u.contains("condition")) {
                    std::string cond = JsonScalarToString(u["condition"]);
                    std::string cur  = JsonScalarToString(ov);
                    bool        eq   = (ToLowerAscii(cur) == ToLowerAscii(cond));
                    if (! eq && (cond == "true" || cond == "false" || cond == "1" || cond == "0")) {
                        eq = (JsonToBool(ov, false) == JsonToBool(u["condition"], false));
                    }
                    node["value"] = eq;
                } else if (node.contains("value")) {
                    applyTypedValue(node["value"], ov);
                } else {
                    node["value"] = ov;
                }
            }
        }
    }

    for (auto it = node.begin(); it != node.end(); ++it) {
        if (it.key() == "user") continue;
        ApplyUserPropertiesRecursive(it.value(), userPropsLower);
    }
}

std::string ApplyUserPropsToSceneSource(const std::string& rawSceneSrc,
                                        const std::string& userPropsJson) {
    if (userPropsJson.empty() || userPropsJson == "{}") return rawSceneSrc;
    try {
        auto props = nlohmann::json::parse(userPropsJson);
        if (! props.is_object() || props.empty()) return rawSceneSrc;

        std::unordered_map<std::string, nlohmann::json> lowerMap;
        for (auto it = props.begin(); it != props.end(); ++it) {
            lowerMap[ToLowerAscii(it.key())] = it.value();
        }

        auto sceneJson = nlohmann::json::parse(rawSceneSrc);
        ApplyUserPropertiesRecursive(sceneJson, lowerMap);
        return sceneJson.dump();
    } catch (const std::exception& ex) {
        LOG_ERROR("ApplyUserPropsToSceneSource warning: %s", ex.what());
        return rawSceneSrc;
    }
}

std::filesystem::path FindVideoFile(const std::filesystem::path& dirPath,
                                    const std::string&           entryFile) {
    if (! entryFile.empty()) {
        auto direct = dirPath / wallpaper::fs::Utf8ToFsPath(entryFile);
        if (std::filesystem::exists(direct) && std::filesystem::is_regular_file(direct))
            return direct;
    }
    static const char* kExts[] = { ".mp4", ".m4v", ".mov", ".wmv", ".webm", ".mkv", ".avi" };
    std::error_code    ec;
    for (const auto& entry : std::filesystem::directory_iterator(dirPath, ec)) {
        if (! entry.is_regular_file(ec)) continue;
        std::string ext = ToLowerAscii(wallpaper::fs::FsPathToUtf8(entry.path().extension()));
        for (const char* k : kExts) {
            if (ext == k) return entry.path();
        }
    }
    return {};
}

} // namespace

RendererEngine::RendererEngine()
    : m_parser(std::make_unique<wallpaper::WPSceneParser>()),
      m_sound(std::make_unique<wallpaper::audio::SoundManager>()),
      m_render(std::make_unique<wallpaper::vulkan::VulkanRender>()),
      m_video(std::make_unique<VideoPlayerWin32>()),
      m_audio(std::make_unique<WasapiAudioAnalyzer>()) {}

RendererEngine::~RendererEngine() { Shutdown(); }

void RendererEngine::SetFrameConsumer(FrameConsumer consumer) {
    m_frameConsumer = std::move(consumer);
    if (m_render) m_render->setFrameCallback(m_frameConsumer);
    if (m_video) m_video->SetFrameConsumer(m_frameConsumer);
}

void RendererEngine::SetPaused(bool paused) {
    m_paused = paused;
    if (m_audio) m_audio->SetPaused(paused || m_isVideo);
}

void RendererEngine::InjectTestAudioTone(float frequencyHz, float amplitude) {
    if (! m_audio) return;
    constexpr std::size_t kSamples   = 256;
    constexpr float       kSampleRate = 48000.0f;
    std::array<float, kSamples> pcm {};
    for (std::size_t i = 0; i < kSamples; ++i) {
        pcm[i] = amplitude * std::sin(2.0f * 3.14159265f * frequencyHz * static_cast<float>(i) / kSampleRate);
    }
    // Feed multiple windows so temporal smoothing converges
    for (int k = 0; k < 8; ++k) {
        m_audio->FeedPcmSamples(pcm.data(), pcm.data(), kSamples);
    }
}

bool RendererEngine::InitVulkanIfNeeded(uint32_t width, uint32_t height, std::string& outError) {
    if (m_render->inited()) {
        if (m_width != width || m_height != height) {
            if (! m_render->resizeOffscreen(width, height)) {
                outError = "Vulkan offscreen resize failed";
                return false;
            }
            m_width  = width;
            m_height = height;
        }
        return true;
    }

    wallpaper::RenderInitInfo info {};
    info.enable_valid_layer = false;
    info.offscreen          = true;
    info.offscreen_tiling   = wallpaper::TexTiling::OPTIMAL;
    info.width              = static_cast<uint16_t>(std::clamp<uint32_t>(width, 64, 4096));
    info.height             = static_cast<uint16_t>(std::clamp<uint32_t>(height, 64, 4096));

    m_render->setFrameCallback(m_frameConsumer);
    if (! m_render->init(info)) {
        outError = "Failed to initialize headless Vulkan device";
        return false;
    }

    m_width  = width;
    m_height = height;
    return true;
}

bool RendererEngine::RebuildSceneFromCachedSource(std::string& outError) {
    if (m_rawSceneSrc.empty()) {
        outError = "No cached scene source available";
        return false;
    }

    auto pVfs = std::make_unique<wallpaper::fs::VFS>();
    if (! pVfs->Mount("/assets", wallpaper::fs::CreatePhysicalFs(m_lastReq.assets), "assets")) {
        outError = "Failed to mount Wallpaper Engine assets directory";
        return false;
    }
    if (! m_pkgPathUtf8.empty()) {
        if (! pVfs->Mount("/assets", wallpaper::fs::WPPkgFs::CreatePkgFs(m_pkgPathUtf8), "pkg")) {
            LOG_ERROR("Failed to mount pkg file: %s", m_pkgPathUtf8.c_str());
        }
    }
    pVfs->Mount("/assets", wallpaper::fs::CreatePhysicalFs(m_lastReq.dir), "dir");

    if (m_rg) {
        m_render->clearLastRenderGraph();
        m_rg.reset();
    }
    m_scene.reset();

    std::string effectiveSrc = ApplyUserPropsToSceneSource(m_rawSceneSrc, m_userPropsJson);
    m_scene                  = m_parser->Parse(m_sceneId, effectiveSrc, *pVfs, *m_sound);
    if (! m_scene) {
        outError = "WPSceneParser failed to parse scene";
        return false;
    }
    m_scene->vfs.swap(pVfs);

    m_rg = wallpaper::sceneToRenderGraph(*m_scene);
    if (! m_rg) {
        outError = "Failed to build render graph from scene";
        return false;
    }

    m_render->compileRenderGraph(*m_scene, *m_rg);
    m_render->UpdateCameraFillMode(*m_scene, wallpaper::FillMode::ASPECTCROP);
    return true;
}

bool RendererEngine::Load(const LoadRequest& req, std::string& outError, std::string& outCode) {
    m_loaded  = false;
    m_isVideo = false;
    outCode   = "LOAD_FAILED";

    try {
        if (req.dir.empty()) {
            outError = "Wallpaper directory is empty";
            return false;
        }

        const auto dirPath = wallpaper::fs::Utf8ToFsPath(req.dir);
        if (! std::filesystem::exists(dirPath)) {
            outError = "Wallpaper directory not found: " + req.dir;
            return false;
        }

        // Parse project.json to determine wallpaper type, entry file, and default properties.
        std::string    wpType    = "scene";
        std::string    entryFile = "scene.json";
        nlohmann::json mergedProps = nlohmann::json::object();

        const auto projPath = dirPath / "project.json";
        if (std::filesystem::exists(projPath)) {
            std::string projText = ReadFileUtf8(projPath);
            if (! projText.empty()) {
                try {
                    auto j = nlohmann::json::parse(projText);
                    if (j.contains("type") && j["type"].is_string()) {
                        std::string t = ToLowerAscii(j["type"].get<std::string>());
                        if (! t.empty()) wpType = t;
                    }
                    if (j.contains("file") && j["file"].is_string()) {
                        std::string f = j["file"].get<std::string>();
                        if (! f.empty()) entryFile = f;
                    }
                    if (j.contains("general") && j["general"].is_object() &&
                        j["general"].contains("properties") &&
                        j["general"]["properties"].is_object()) {
                        for (auto it = j["general"]["properties"].begin();
                             it != j["general"]["properties"].end();
                             ++it) {
                            if (it.value().is_object() && it.value().contains("value")) {
                                mergedProps[it.key()] = it.value()["value"];
                            }
                        }
                    }
                } catch (const std::exception& ex) {
                    LOG_ERROR("project.json parse warning: %s", ex.what());
                }
            }
        }

        // Merge any caller-supplied property overrides from load request.
        if (! req.initialPropsJson.empty()) {
            try {
                auto overrides = nlohmann::json::parse(req.initialPropsJson);
                if (overrides.is_object()) {
                    for (auto it = overrides.begin(); it != overrides.end(); ++it) {
                        mergedProps[it.key()] = it.value();
                    }
                }
            } catch (const std::exception& ex) {
                LOG_ERROR("initial props parse warning: %s", ex.what());
            }
        }

        m_lastReq       = req;
        m_userPropsJson = mergedProps.dump();

        // ---- Native Video Wallpaper Path (Windows Media Foundation x64) ----
        if (wpType == "video") {
            if (m_rg) {
                m_render->clearLastRenderGraph();
                m_rg.reset();
            }
            m_scene.reset();
            m_rawSceneSrc.clear();
            m_pkgPathUtf8.clear();
            if (m_audio) m_audio->SetPaused(true);

            auto videoPath = FindVideoFile(dirPath, entryFile);
            if (videoPath.empty()) {
                outCode  = "LOAD_FAILED";
                outError = "Video file not found in wallpaper directory: " + entryFile;
                return false;
            }

            m_video->SetFrameConsumer(m_frameConsumer);
            std::string videoUtf8 = wallpaper::fs::FsPathToUtf8(videoPath);
            if (! m_video->Open(videoUtf8, req.width, req.height, outError)) {
                outCode = "VIDEO_OPEN_FAILED";
                return false;
            }

            m_width   = req.width;
            m_height  = req.height;
            m_isVideo = true;
            m_loaded  = true;
            outCode.clear();
            return true;
        }

        if (wpType != "scene") {
            outCode  = "UNSUPPORTED_TYPE";
            outError = "Wallpaper type '" + wpType + "' is not supported (Scene/Video supported)";
            return false;
        }

        // ---- Scene Wallpaper Path (Headless Vulkan + WASAPI Audio FFT) ----
        m_video->Close();

        if (req.assets.empty()) {
            outError = "Wallpaper Engine assets directory is empty";
            return false;
        }
        const auto assetsPath = wallpaper::fs::Utf8ToFsPath(req.assets);
        if (! std::filesystem::exists(assetsPath)) {
            outError = "Wallpaper Engine assets directory not found: " + req.assets;
            return false;
        }

        if (! InitVulkanIfNeeded(req.width, req.height, outError)) {
            outCode = "VULKAN_INIT_FAILED";
            return false;
        }

        if (! m_sound->IsInited()) {
            m_sound->Init();
        } else {
            m_sound->UnMountAll();
        }

        std::filesystem::path entryFs = wallpaper::fs::Utf8ToFsPath(entryFile);
        std::filesystem::path pkgFs   = dirPath / entryFs;
        pkgFs.replace_extension(".pkg");
        if (! std::filesystem::exists(pkgFs)) {
            std::filesystem::path fallbackPkg = dirPath / "scene.pkg";
            if (std::filesystem::exists(fallbackPkg)) pkgFs = fallbackPkg;
        }
        m_pkgPathUtf8 = std::filesystem::exists(pkgFs) ? wallpaper::fs::FsPathToUtf8(pkgFs) : "";

        // Probe VFS to read raw scene.json once and cache it for live `props` updates.
        {
            auto probeVfs = std::make_unique<wallpaper::fs::VFS>();
            if (! probeVfs->Mount("/assets", wallpaper::fs::CreatePhysicalFs(req.assets), "assets")) {
                outError = "Failed to mount Wallpaper Engine assets directory";
                return false;
            }
            if (! m_pkgPathUtf8.empty()) {
                probeVfs->Mount("/assets", wallpaper::fs::WPPkgFs::CreatePkgFs(m_pkgPathUtf8), "pkg");
            }
            probeVfs->Mount("/assets", wallpaper::fs::CreatePhysicalFs(req.dir), "dir");

            std::string entryName = wallpaper::fs::FsPathToUtf8(entryFs.filename());
            std::string scenePath = "/assets/" + entryName;
            if (! probeVfs->Contains(scenePath)) scenePath = "/assets/scene.json";

            m_rawSceneSrc.clear();
            if (probeVfs->Contains(scenePath)) {
                auto f = probeVfs->Open(scenePath);
                if (f) m_rawSceneSrc = f->ReadAllStr();
            }
            if (m_rawSceneSrc.empty()) {
                outError = "scene.json not found in wallpaper package or directory";
                return false;
            }
        }

        m_sceneId = wallpaper::fs::FsPathToUtf8(dirPath.filename());
        if (! RebuildSceneFromCachedSource(outError)) {
            return false;
        }

        if (m_audio) {
            m_audio->SetPaused(m_paused);
            m_audio->Start();
        }

        m_loaded = true;
        outCode.clear();
        return true;
    } catch (const std::exception& ex) {
        outError = std::string("Exception during scene load: ") + ex.what();
        return false;
    } catch (...) {
        outError = "Unknown exception during scene load";
        return false;
    }
}

bool RendererEngine::Resize(uint32_t width, uint32_t height, std::string& outError) {
    if (m_isVideo) {
        if (! m_video || ! m_video->IsOpen()) {
            outError = "Video renderer not initialized";
            return false;
        }
        if (width == m_width && height == m_height) return true;
        if (! m_video->Resize(width, height)) {
            outError = "Video resize failed";
            return false;
        }
        m_width  = width;
        m_height = height;
        return true;
    }

    if (! m_render || ! m_render->inited()) {
        outError = "Renderer not initialized";
        return false;
    }
    if (width == m_width && height == m_height) {
        return true;
    }

    try {
        if (m_rg) {
            m_render->clearLastRenderGraph();
            m_rg.reset();
        }
        if (! m_render->resizeOffscreen(width, height)) {
            outError = "Failed to resize Vulkan offscreen swapchain";
            return false;
        }
        m_width  = width;
        m_height = height;

        if (m_scene) {
            m_rg = wallpaper::sceneToRenderGraph(*m_scene);
            if (! m_rg) {
                outError = "Failed to rebuild render graph on resize";
                return false;
            }
            m_render->compileRenderGraph(*m_scene, *m_rg);
            m_render->UpdateCameraFillMode(*m_scene, wallpaper::FillMode::ASPECTCROP);
        }
        return true;
    } catch (const std::exception& ex) {
        outError = std::string("Exception during resize: ") + ex.what();
        return false;
    }
}

bool RendererEngine::SetProperties(const std::string& propsJson, std::string& outError) {
    try {
        auto incoming = nlohmann::json::parse(propsJson);
        if (! incoming.is_object()) {
            outError = "props.values must be a JSON object";
            return false;
        }
        auto merged = m_userPropsJson.empty() ? nlohmann::json::object()
                                              : nlohmann::json::parse(m_userPropsJson);
        if (! merged.is_object()) merged = nlohmann::json::object();
        for (auto it = incoming.begin(); it != incoming.end(); ++it) {
            merged[it.key()] = it.value();
        }
        m_userPropsJson = merged.dump();

        if (! m_loaded || m_isVideo || m_rawSceneSrc.empty()) {
            return true;
        }
        return RebuildSceneFromCachedSource(outError);
    } catch (const std::exception& ex) {
        outError = std::string("Failed to apply properties: ") + ex.what();
        return false;
    }
}

bool RendererEngine::RenderFrame(double deltaSec) {
    if (! m_loaded) return false;

    if (m_isVideo) {
        return m_video && m_video->RenderFrame(deltaSec);
    }

    if (! m_scene || ! m_render || ! m_rg) return false;
    try {
        if (m_audio && m_scene->shaderValueUpdater) {
            auto* wpUpdater =
                dynamic_cast<wallpaper::WPShaderValueUpdater*>(m_scene->shaderValueUpdater.get());
            if (wpUpdater) {
                AudioSpectrumData spec {};
                m_audio->GetSpectrum(spec);
                wpUpdater->SetAudioSpectrum(
                    spec.left64, spec.right64, spec.left32, spec.right32, spec.left16, spec.right16);
            }
        }

        m_scene->shaderValueUpdater->FrameBegin();
        m_scene->shaderValueUpdater->MouseInput(0.5f, 0.5f);
        m_scene->paritileSys->Emitt();

        m_render->drawFrame(*m_scene);

        m_scene->PassFrameTime(deltaSec);
        m_scene->shaderValueUpdater->FrameEnd();
        return true;
    } catch (const std::exception& ex) {
        LOG_ERROR("RenderFrame exception: %s", ex.what());
        return false;
    }
}

void RendererEngine::Shutdown() {
    m_loaded  = false;
    m_isVideo = false;
    if (m_audio) {
        m_audio->Stop();
    }
    if (m_video) {
        m_video->Close();
    }
    if (m_render && m_render->inited()) {
        m_render->clearLastRenderGraph();
    }
    m_rg.reset();
    m_scene.reset();
    if (m_render && m_render->inited()) {
        m_render->destroy();
    }
}

} // namespace lcdfusion_wpe

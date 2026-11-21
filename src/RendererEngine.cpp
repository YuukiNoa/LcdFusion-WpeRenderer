#include "RendererEngine.hpp"

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

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <filesystem>

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

} // namespace

RendererEngine::RendererEngine()
    : m_parser(std::make_unique<wallpaper::WPSceneParser>()),
      m_sound(std::make_unique<wallpaper::audio::SoundManager>()),
      m_render(std::make_unique<wallpaper::vulkan::VulkanRender>()) {}

RendererEngine::~RendererEngine() { Shutdown(); }

void RendererEngine::SetFrameConsumer(FrameConsumer consumer) {
    m_frameConsumer = std::move(consumer);
    if (m_render) {
        m_render->setFrameCallback(m_frameConsumer);
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

bool RendererEngine::Load(const LoadRequest& req, std::string& outError, std::string& outCode) {
    m_loaded = false;
    outCode  = "LOAD_FAILED";

    try {
        if (req.dir.empty()) {
            outError = "Wallpaper directory is empty";
            return false;
        }
        if (req.assets.empty()) {
            outError = "Wallpaper Engine assets directory is empty";
            return false;
        }

        const auto dirPath    = wallpaper::fs::Utf8ToFsPath(req.dir);
        const auto assetsPath = wallpaper::fs::Utf8ToFsPath(req.assets);
        if (! std::filesystem::exists(dirPath)) {
            outError = "Wallpaper directory not found: " + req.dir;
            return false;
        }
        if (! std::filesystem::exists(assetsPath)) {
            outError = "Wallpaper Engine assets directory not found: " + req.assets;
            return false;
        }

        // Parse project.json to validate wallpaper type and entry file.
        std::string entryFile = "scene.json";
        const auto  projPath  = dirPath / "project.json";
        if (std::filesystem::exists(projPath)) {
            std::string projText = ReadFileUtf8(projPath);
            if (! projText.empty()) {
                try {
                    auto j = nlohmann::json::parse(projText);
                    if (j.contains("type") && j["type"].is_string()) {
                        std::string type = ToLowerAscii(j["type"].get<std::string>());
                        if (! type.empty() && type != "scene") {
                            outCode  = "UNSUPPORTED_TYPE";
                            outError = "Wallpaper type '" + type + "' is not a Scene wallpaper";
                            return false;
                        }
                    }
                    if (j.contains("file") && j["file"].is_string()) {
                        std::string f = j["file"].get<std::string>();
                        if (! f.empty()) entryFile = f;
                    }
                } catch (const std::exception& ex) {
                    LOG_ERROR("project.json parse warning: %s", ex.what());
                }
            }
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

        // Build VFS: shared assets (lowest) -> scene.pkg -> loose wallpaper dir files (highest).
        auto pVfs = std::make_unique<wallpaper::fs::VFS>();
        if (! pVfs->Mount("/assets", wallpaper::fs::CreatePhysicalFs(req.assets), "assets")) {
            outError = "Failed to mount Wallpaper Engine assets directory";
            return false;
        }

        std::filesystem::path entryFs = wallpaper::fs::Utf8ToFsPath(entryFile);
        std::filesystem::path pkgFs   = dirPath / entryFs;
        pkgFs.replace_extension(".pkg");

        if (! std::filesystem::exists(pkgFs)) {
            // Fallback: look for scene.pkg or any .pkg inside dir
            std::filesystem::path fallbackPkg = dirPath / "scene.pkg";
            if (std::filesystem::exists(fallbackPkg)) {
                pkgFs = fallbackPkg;
            }
        }

        if (std::filesystem::exists(pkgFs)) {
            std::string pkgUtf8 = wallpaper::fs::FsPathToUtf8(pkgFs);
            if (! pVfs->Mount("/assets", wallpaper::fs::WPPkgFs::CreatePkgFs(pkgUtf8), "pkg")) {
                LOG_ERROR("Failed to mount pkg file: %s", pkgUtf8.c_str());
            }
        }

        // Mount physical directory on top so loose files (e.g. custom shaders/) override/supplement pkg.
        pVfs->Mount("/assets", wallpaper::fs::CreatePhysicalFs(req.dir), "dir");

        std::string entryName = wallpaper::fs::FsPathToUtf8(entryFs.filename());
        std::string scenePath = "/assets/" + entryName;
        if (! pVfs->Contains(scenePath)) {
            scenePath = "/assets/scene.json";
        }

        std::string sceneSrc;
        if (pVfs->Contains(scenePath)) {
            auto f = pVfs->Open(scenePath);
            if (f) sceneSrc = f->ReadAllStr();
        }
        if (sceneSrc.empty()) {
            outError = "scene.json not found in wallpaper package or directory";
            return false;
        }

        std::string sceneId = wallpaper::fs::FsPathToUtf8(dirPath.filename());

        // Clear any previous render graph before replacing scene.
        if (m_rg) {
            m_render->clearLastRenderGraph();
            m_rg.reset();
        }
        m_scene.reset();

        m_scene = m_parser->Parse(sceneId, sceneSrc, *pVfs, *m_sound);
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

bool RendererEngine::RenderFrame(double deltaSec) {
    if (! m_loaded || ! m_scene || ! m_render || ! m_rg) return false;
    try {
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
    m_loaded = false;
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

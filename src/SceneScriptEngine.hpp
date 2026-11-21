#pragma once

#include <memory>
#include <string>

namespace wallpaper
{
class Scene;
}

namespace lcdfusion_wpe
{

struct AudioSpectrumData;
class TextLayerRenderer;

// Embedded QuickJS-ng ES2023/ESM runtime implementing Wallpaper Engine SceneScript bindings
// (WEColor, WEMath, WEVector, Vec2/Vec3, createScriptProperties, engine/audio buffers, thisLayer/thisScene).
class SceneScriptEngine {
public:
    SceneScriptEngine();
    ~SceneScriptEngine();

    SceneScriptEngine(const SceneScriptEngine&)            = delete;
    SceneScriptEngine& operator=(const SceneScriptEngine&) = delete;

    void Reset();

    bool Load(const std::string& effectiveSceneJson, const std::string& userPropsJson,
              wallpaper::Scene& scene, TextLayerRenderer& textRenderer);

    void Update(double deltaSec, double elapsedSec, const AudioSpectrumData& audio,
                wallpaper::Scene& scene);

    void ApplyUserProperties(const std::string& changedPropsJson);

    std::size_t ActiveScriptCount() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace lcdfusion_wpe

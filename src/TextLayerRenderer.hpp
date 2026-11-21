#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace wallpaper
{
namespace fs
{
class VFS;
}
} // namespace wallpaper

namespace lcdfusion_wpe
{

struct TextLayerState {
    int                  id { 0 };
    std::string          name;
    std::string          text;
    std::string          font;
    std::string          horizontalAlign { "center" };
    std::string          verticalAlign { "center" };
    std::array<float, 3> origin { 0.0f, 0.0f, 0.0f };
    std::array<float, 3> scale { 1.0f, 1.0f, 1.0f };
    std::array<float, 3> angles { 0.0f, 0.0f, 0.0f };
    std::array<float, 2> size { 256.0f, 64.0f };
    std::array<float, 3> color { 1.0f, 1.0f, 1.0f };
    std::array<float, 3> backgroundColor { 0.0f, 0.0f, 0.0f };
    float                pointSize { 24.0f };
    float                alpha { 1.0f };
    float                padding { 0.0f };
    bool                 visible { true };
    bool                 opaqueBackground { false };
};

// Loads Wallpaper Engine `text` objects + embedded TTF/OTF fonts from VFS and composites them
// onto the rendered BGRA32 frame using Windows GDI+ with exact scene ortho-to-camera projection.
class TextLayerRenderer {
public:
    TextLayerRenderer();
    ~TextLayerRenderer();

    TextLayerRenderer(const TextLayerRenderer&)            = delete;
    TextLayerRenderer& operator=(const TextLayerRenderer&) = delete;

    void Clear();
    bool LoadFromSceneJson(const std::string& effectiveSceneJson, wallpaper::fs::VFS& vfs);

    bool HasVisibleLayers() const;

    std::vector<TextLayerState>&       Layers() { return m_layers; }
    const std::vector<TextLayerState>& Layers() const { return m_layers; }

    TextLayerState* FindById(int id);
    TextLayerState* FindByName(const std::string& name);

    void CompositeOntoBgra(uint8_t* bgra, uint32_t outW, uint32_t outH, uint32_t stride,
                           double orthoW, double orthoH, double camW, double camH);

private:
    struct Impl;
    std::unique_ptr<Impl>       m_impl;
    std::vector<TextLayerState> m_layers;
};

} // namespace lcdfusion_wpe

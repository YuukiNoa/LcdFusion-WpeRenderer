#include "TextLayerRenderer.hpp"

#include "Fs/VFS.h"
#include "Utils/Logging.h"

#include <nlohmann/json.hpp>

#ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <sstream>
#include <unordered_map>

namespace lcdfusion_wpe
{

namespace
{

std::string ToLowerStr(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::wstring Utf8ToWideText(const std::string& s) {
    if (s.empty()) return {};
    int needed = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (needed <= 0) return {};
    std::wstring out(static_cast<std::size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), needed);
    return out;
}

const nlohmann::json* UnwrapValue(const nlohmann::json& obj, const char* key) {
    if (! obj.is_object() || ! obj.contains(key)) return nullptr;
    const auto& v = obj.at(key);
    if (v.is_null()) return nullptr;
    if (v.is_object() && v.contains("value")) return &v.at("value");
    return &v;
}

std::string ReadStringField(const nlohmann::json& obj, const char* key, const std::string& def) {
    const auto* v = UnwrapValue(obj, key);
    if (! v || v->is_null()) return def;
    if (v->is_string()) return v->get<std::string>();
    return def;
}

float ReadFloatField(const nlohmann::json& obj, const char* key, float def) {
    const auto* v = UnwrapValue(obj, key);
    if (! v || v->is_null()) return def;
    if (v->is_number()) return static_cast<float>(v->get<double>());
    if (v->is_string()) {
        try { return std::stof(v->get<std::string>()); } catch (...) { return def; }
    }
    return def;
}

bool ReadBoolField(const nlohmann::json& obj, const char* key, bool def) {
    const auto* v = UnwrapValue(obj, key);
    if (! v || v->is_null()) return def;
    if (v->is_boolean()) return v->get<bool>();
    if (v->is_number()) return v->get<double>() != 0.0;
    if (v->is_string()) {
        std::string s = ToLowerStr(v->get<std::string>());
        if (s == "true" || s == "1") return true;
        if (s == "false" || s == "0") return false;
    }
    return def;
}

template<std::size_t N>
std::array<float, N> ReadVecField(const nlohmann::json& obj, const char* key,
                                  std::array<float, N> def) {
    const auto* v = UnwrapValue(obj, key);
    if (! v || v->is_null()) return def;
    if (v->is_number()) {
        float f = static_cast<float>(v->get<double>());
        def.fill(f);
        return def;
    }
    if (v->is_string()) {
        std::istringstream iss(v->get<std::string>());
        for (std::size_t i = 0; i < N; ++i) {
            float val = def[i];
            if (! (iss >> val)) break;
            def[i] = val;
        }
    }
    return def;
}

std::wstring SystemFontFallback(const std::string& fontSpec) {
    std::string lo = ToLowerStr(fontSpec);
    if (lo.find("mono") != std::string::npos || lo.find("consol") != std::string::npos)
        return L"Consolas";
    if (lo.find("serif") != std::string::npos && lo.find("sans") == std::string::npos)
        return L"Georgia";
    return L"Segoe UI";
}

} // namespace

struct TextLayerRenderer::Impl {
    ULONG_PTR                                        gdiplusToken { 0 };
    bool                                             gdiplusOk { false };
    std::unique_ptr<Gdiplus::PrivateFontCollection>  fontCollection;
    std::unordered_map<std::string, std::wstring>    fontFamilyMap;
    std::vector<std::vector<uint8_t>>                fontBuffers;

    Impl() {
        Gdiplus::GdiplusStartupInput input;
        if (Gdiplus::GdiplusStartup(&gdiplusToken, &input, nullptr) == Gdiplus::Ok) {
            gdiplusOk = true;
        }
    }

    ~Impl() {
        fontCollection.reset();
        fontBuffers.clear();
        if (gdiplusOk) {
            Gdiplus::GdiplusShutdown(gdiplusToken);
            gdiplusOk = false;
        }
    }

    void ResetFonts() {
        fontFamilyMap.clear();
        fontCollection.reset();
        fontBuffers.clear();
        if (gdiplusOk) {
            fontCollection = std::make_unique<Gdiplus::PrivateFontCollection>();
        }
    }

    void EnsureFontLoaded(const std::string& fontPath, wallpaper::fs::VFS& vfs) {
        if (! gdiplusOk || fontPath.empty()) return;
        if (fontFamilyMap.count(fontPath) > 0) return;

        std::string lo = ToLowerStr(fontPath);
        if (lo.rfind("systemfont_", 0) == 0) {
            fontFamilyMap[fontPath] = SystemFontFallback(fontPath);
            return;
        }

        std::string vfsPath = "/assets/" + fontPath;
        if (! vfs.Contains(vfsPath)) {
            fontFamilyMap[fontPath] = SystemFontFallback(fontPath);
            return;
        }

        auto stream = vfs.Open(vfsPath);
        if (! stream) {
            fontFamilyMap[fontPath] = SystemFontFallback(fontPath);
            return;
        }

        std::string bytes = stream->ReadAllStr();
        if (bytes.empty()) {
            fontFamilyMap[fontPath] = SystemFontFallback(fontPath);
            return;
        }

        // Load into PrivateFontCollection and identify the newly registered family name.
        if (fontCollection) {
            int beforeCount = fontCollection->GetFamilyCount();
            std::vector<Gdiplus::FontFamily> beforeFams(static_cast<std::size_t>(std::max(0, beforeCount)));
            int beforeGot = 0;
            if (beforeCount > 0) {
                fontCollection->GetFamilies(beforeCount, beforeFams.data(), &beforeGot);
            }

            std::vector<uint8_t> buf(bytes.begin(), bytes.end());
            auto st = fontCollection->AddMemoryFont(buf.data(), static_cast<INT>(buf.size()));
            if (st == Gdiplus::Ok) {
                fontBuffers.push_back(std::move(buf));
                int afterCount = fontCollection->GetFamilyCount();
                if (afterCount > 0) {
                    std::vector<Gdiplus::FontFamily> afterFams(static_cast<std::size_t>(afterCount));
                    int afterGot = 0;
                    fontCollection->GetFamilies(afterCount, afterFams.data(), &afterGot);
                    // Pick a newly added family name, or the last one if duplicate family.
                    std::wstring chosen;
                    for (int i = afterGot - 1; i >= 0; --i) {
                        WCHAR name[LF_FACESIZE] = {};
                        if (afterFams[static_cast<std::size_t>(i)].GetFamilyName(name) == Gdiplus::Ok && name[0] != 0) {
                            bool existed = false;
                            for (int j = 0; j < beforeGot; ++j) {
                                WCHAR bname[LF_FACESIZE] = {};
                                if (beforeFams[static_cast<std::size_t>(j)].GetFamilyName(bname) == Gdiplus::Ok &&
                                    wcscmp(name, bname) == 0) {
                                    existed = true;
                                    break;
                                }
                            }
                            if (! existed) {
                                chosen = name;
                                break;
                            }
                            if (chosen.empty()) chosen = name;
                        }
                    }
                    if (! chosen.empty()) {
                        fontFamilyMap[fontPath] = chosen;
                        return;
                    }
                }
            }
        }

        fontFamilyMap[fontPath] = SystemFontFallback(fontPath);
    }

    std::unique_ptr<Gdiplus::Font> CreateFontForLayer(const TextLayerState& layer, float pxSize) const {
        std::wstring famName = SystemFontFallback(layer.font);
        auto         it      = fontFamilyMap.find(layer.font);
        if (it != fontFamilyMap.end() && ! it->second.empty()) {
            famName = it->second;
        }

        // Try private font collection first for embedded TTF/OTF fonts.
        if (fontCollection) {
            Gdiplus::FontFamily fam(famName.c_str(), fontCollection.get());
            if (fam.GetLastStatus() == Gdiplus::Ok) {
                INT style = Gdiplus::FontStyleRegular;
                if (! fam.IsStyleAvailable(style)) {
                    if (fam.IsStyleAvailable(Gdiplus::FontStyleBold))
                        style = Gdiplus::FontStyleBold;
                    else if (fam.IsStyleAvailable(Gdiplus::FontStyleItalic))
                        style = Gdiplus::FontStyleItalic;
                }
                auto f = std::make_unique<Gdiplus::Font>(&fam, pxSize, style, Gdiplus::UnitPixel);
                if (f->GetLastStatus() == Gdiplus::Ok) return f;
            }
        }

        // Fallback to installed system font
        Gdiplus::FontFamily sysFam(famName.c_str());
        if (sysFam.GetLastStatus() == Gdiplus::Ok) {
            auto f = std::make_unique<Gdiplus::Font>(
                &sysFam, pxSize, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
            if (f->GetLastStatus() == Gdiplus::Ok) return f;
        }

        Gdiplus::FontFamily defFam(L"Segoe UI");
        return std::make_unique<Gdiplus::Font>(
            &defFam, pxSize, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
    }
};

TextLayerRenderer::TextLayerRenderer(): m_impl(std::make_unique<Impl>()) {}
TextLayerRenderer::~TextLayerRenderer() = default;

void TextLayerRenderer::Clear() {
    m_layers.clear();
    if (m_impl) m_impl->ResetFonts();
}

bool TextLayerRenderer::LoadFromSceneJson(const std::string& effectiveSceneJson,
                                          wallpaper::fs::VFS& vfs) {
    Clear();
    if (effectiveSceneJson.empty()) return true;

    try {
        auto j = nlohmann::json::parse(effectiveSceneJson);
        if (! j.is_object() || ! j.contains("objects") || ! j["objects"].is_array())
            return true;

        for (const auto& obj : j["objects"]) {
            if (! obj.is_object() || ! obj.contains("text") || obj["text"].is_null())
                continue;

            TextLayerState layer {};
            layer.id              = obj.contains("id") && obj["id"].is_number_integer() ? obj["id"].get<int>() : 0;
            layer.name            = ReadStringField(obj, "name", "");
            layer.text            = ReadStringField(obj, "text", "");
            layer.font            = ReadStringField(obj, "font", "systemfont_sansserif");
            layer.horizontalAlign = ToLowerStr(ReadStringField(obj, "horizontalalign", "center"));
            layer.verticalAlign   = ToLowerStr(ReadStringField(obj, "verticalalign", "center"));
            layer.origin          = ReadVecField<3>(obj, "origin", { 0.0f, 0.0f, 0.0f });
            layer.scale           = ReadVecField<3>(obj, "scale", { 1.0f, 1.0f, 1.0f });
            layer.angles          = ReadVecField<3>(obj, "angles", { 0.0f, 0.0f, 0.0f });
            layer.size            = ReadVecField<2>(obj, "size", { 256.0f, 64.0f });
            layer.color           = ReadVecField<3>(obj, "color", { 1.0f, 1.0f, 1.0f });
            layer.backgroundColor = ReadVecField<3>(obj, "backgroundcolor", { 0.0f, 0.0f, 0.0f });
            layer.pointSize       = ReadFloatField(obj, "pointsize", 24.0f);
            layer.alpha           = ReadFloatField(obj, "alpha", 1.0f);
            layer.padding         = ReadFloatField(obj, "padding", 0.0f);
            layer.visible         = ReadBoolField(obj, "visible", true);
            layer.opaqueBackground = ReadBoolField(obj, "opaquebackground", false);

            if (m_impl) m_impl->EnsureFontLoaded(layer.font, vfs);
            m_layers.push_back(std::move(layer));
        }
        return true;
    } catch (const std::exception& ex) {
        LOG_ERROR("TextLayerRenderer::LoadFromSceneJson warning: %s", ex.what());
        return false;
    }
}

bool TextLayerRenderer::HasVisibleLayers() const {
    for (const auto& l : m_layers) {
        if (l.visible && l.alpha > 0.005f && ! l.text.empty()) return true;
    }
    return false;
}

TextLayerState* TextLayerRenderer::FindById(int id) {
    if (id == 0) return nullptr;
    for (auto& l : m_layers) {
        if (l.id == id) return &l;
    }
    return nullptr;
}

TextLayerState* TextLayerRenderer::FindByName(const std::string& name) {
    if (name.empty()) return nullptr;
    for (auto& l : m_layers) {
        if (l.name == name) return &l;
    }
    return nullptr;
}

void TextLayerRenderer::CompositeOntoBgra(uint8_t* bgra, uint32_t outW, uint32_t outH,
                                          uint32_t stride, double orthoW, double orthoH,
                                          double camW, double camH) {
    if (! m_impl || ! m_impl->gdiplusOk || ! bgra || outW == 0 || outH == 0) return;
    if (! HasVisibleLayers()) return;
    if (camW <= 1.0 || camH <= 1.0) return;

    Gdiplus::Bitmap bmp(static_cast<INT>(outW),
                        static_cast<INT>(outH),
                        static_cast<INT>(stride),
                        PixelFormat32bppARGB,
                        bgra);
    Gdiplus::Graphics g(&bmp);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);
    g.SetCompositingMode(Gdiplus::CompositingModeSourceOver);

    const double camLeft   = orthoW * 0.5 - camW * 0.5;
    const double camTop    = orthoH * 0.5 + camH * 0.5;
    const double pxPerUnitX = static_cast<double>(outW) / camW;
    const double pxPerUnitY = static_cast<double>(outH) / camH;

    for (const auto& layer : m_layers) {
        if (! layer.visible || layer.alpha <= 0.005f || layer.text.empty()) continue;

        const double sx = (static_cast<double>(layer.origin[0]) - camLeft) * pxPerUnitX;
        const double sy = (camTop - static_cast<double>(layer.origin[1])) * pxPerUnitY;

        const float scaleX = std::max(0.01f, std::abs(layer.scale[0]));
        const float scaleY = std::max(0.01f, std::abs(layer.scale[1]));
        const float boxW   = std::max(16.0f, static_cast<float>(layer.size[0] * pxPerUnitX * scaleX));
        const float boxH   = std::max(16.0f, static_cast<float>(layer.size[1] * pxPerUnitY * scaleY));

        // Wallpaper Engine pointsize scales with scene ortho pixels (approx 4/3 px per pt at 96 DPI).
        float fontPx = static_cast<float>(std::max(1.0f, layer.pointSize) * 1.3333 * pxPerUnitY * scaleY);
        fontPx       = std::clamp(fontPx, 6.0f, static_cast<float>(outH));

        auto font = m_impl->CreateFontForLayer(layer, fontPx);
        if (! font) continue;

        auto toByte = [](float c) -> BYTE {
            return static_cast<BYTE>(std::clamp(std::lround(c * 255.0f), 0L, 255L));
        };
        BYTE a = toByte(layer.alpha);
        BYTE r = toByte(layer.color[0]);
        BYTE gr = toByte(layer.color[1]);
        BYTE b = toByte(layer.color[2]);

        Gdiplus::GraphicsState st = g.Save();
        g.TranslateTransform(static_cast<Gdiplus::REAL>(sx), static_cast<Gdiplus::REAL>(sy));
        if (std::abs(layer.angles[2]) > 1e-4f) {
            float deg = -layer.angles[2] * 57.2957795f;
            g.RotateTransform(deg);
        }

        // Compute draw rect relative to layer origin according to horizontal/vertical alignment.
        Gdiplus::StringFormat fmt;
        float rectX = -boxW * 0.5f;
        float rectY = -boxH * 0.5f;

        if (layer.horizontalAlign == "left") {
            fmt.SetAlignment(Gdiplus::StringAlignmentNear);
            rectX = 0.0f;
        } else if (layer.horizontalAlign == "right") {
            fmt.SetAlignment(Gdiplus::StringAlignmentFar);
            rectX = -boxW;
        } else {
            fmt.SetAlignment(Gdiplus::StringAlignmentCenter);
            rectX = -boxW * 0.5f;
        }

        if (layer.verticalAlign == "top") {
            fmt.SetLineAlignment(Gdiplus::StringAlignmentNear);
            rectY = 0.0f;
        } else if (layer.verticalAlign == "bottom") {
            fmt.SetLineAlignment(Gdiplus::StringAlignmentFar);
            rectY = -boxH;
        } else {
            fmt.SetLineAlignment(Gdiplus::StringAlignmentCenter);
            rectY = -boxH * 0.5f;
        }

        // Generous layout box so slightly wider glyphs don't clip unnecessarily.
        Gdiplus::RectF rect(rectX, rectY, boxW, boxH);
        if (layer.opaqueBackground) {
            Gdiplus::SolidBrush bgBrush(Gdiplus::Color(
                a, toByte(layer.backgroundColor[0]), toByte(layer.backgroundColor[1]), toByte(layer.backgroundColor[2])));
            g.FillRectangle(&bgBrush, rect);
        }

        fmt.SetFormatFlags(Gdiplus::StringFormatFlagsNoClip);

        std::wstring wtext = Utf8ToWideText(layer.text);
        Gdiplus::SolidBrush textBrush(Gdiplus::Color(a, r, gr, b));
        g.DrawString(wtext.c_str(), -1, font.get(), rect, &fmt, &textBrush);

        g.Restore(st);
    }
}

} // namespace lcdfusion_wpe

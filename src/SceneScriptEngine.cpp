#include "SceneScriptEngine.hpp"
#include "AudioSpectrumWin32.hpp"
#include "TextLayerRenderer.hpp"

#include "Scene/Scene.h"
#include "Scene/SceneCamera.h"
#include "Scene/SceneImageEffectLayer.h"
#include "Scene/SceneNode.h"
#include "Scene/SceneShader.h"
#include "Utils/Logging.h"
#include "WPShaderValueUpdater.hpp"

#include <quickjs.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstring>
#include <sstream>
#include <unordered_map>
#include <vector>

namespace lcdfusion_wpe
{

namespace
{

std::string ToLowerJs(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// Built-in ES module sources for Wallpaper Engine SceneScript standard library.
constexpr const char* kModWEColor = R"JS(
export function hsv2rgb(hsv) {
    let h = 0, s = 0, v = 0;
    if (hsv && typeof hsv === 'object') {
        h = Number(hsv.x !== undefined ? hsv.x : (hsv.r !== undefined ? hsv.r : (hsv[0] || 0)));
        s = Number(hsv.y !== undefined ? hsv.y : (hsv.g !== undefined ? hsv.g : (hsv[1] || 0)));
        v = Number(hsv.z !== undefined ? hsv.z : (hsv.b !== undefined ? hsv.b : (hsv[2] || 0)));
    }
    h = ((h % 1) + 1) % 1;
    s = Math.max(0, Math.min(1, s));
    v = Math.max(0, Math.min(1, v));
    let i = Math.floor(h * 6);
    let f = h * 6 - i;
    let p = v * (1 - s);
    let q = v * (1 - f * s);
    let t = v * (1 - (1 - f) * s);
    let r = 0, g = 0, b = 0;
    switch (i % 6) {
        case 0: r = v; g = t; b = p; break;
        case 1: r = q; g = v; b = p; break;
        case 2: r = p; g = v; b = t; break;
        case 3: r = p; g = q; b = v; break;
        case 4: r = t; g = p; b = v; break;
        case 5: r = v; g = p; b = q; break;
    }
    return new Vec3(r, g, b);
}

export function rgb2hsv(rgb) {
    let r = 0, g = 0, b = 0;
    if (rgb && typeof rgb === 'object') {
        r = Number(rgb.x !== undefined ? rgb.x : (rgb.r !== undefined ? rgb.r : (rgb[0] || 0)));
        g = Number(rgb.y !== undefined ? rgb.y : (rgb.g !== undefined ? rgb.g : (rgb[1] || 0)));
        b = Number(rgb.z !== undefined ? rgb.z : (rgb.b !== undefined ? rgb.b : (rgb[2] || 0)));
    }
    let max = Math.max(r, g, b), min = Math.min(r, g, b);
    let d = max - min;
    let h = 0;
    let s = max === 0 ? 0 : d / max;
    let v = max;
    if (d !== 0) {
        switch (max) {
            case r: h = ((g - b) / d + (g < b ? 6 : 0)) / 6; break;
            case g: h = ((b - r) / d + 2) / 6; break;
            case b: h = ((r - g) / d + 4) / 6; break;
        }
    }
    return new Vec3(h, s, v);
}

export function normalizeColor(c) {
    if (typeof c === 'string') {
        let p = c.trim().split(/\s+/).map(Number);
        return new Vec3(p[0] || 0, p[1] || 0, p[2] || 0);
    }
    if (c && typeof c === 'object') {
        return new Vec3(c.x ?? c.r ?? c[0] ?? 0, c.y ?? c.g ?? c[1] ?? 0, c.z ?? c.b ?? c[2] ?? 0);
    }
    return new Vec3(1, 1, 1);
}
export default { hsv2rgb, rgb2hsv, normalizeColor };
)JS";

constexpr const char* kModWEMath = R"JS(
export const deg2rad = Math.PI / 180.0;
export const rad2deg = 180.0 / Math.PI;
export function mix(a, b, t) {
    if (a instanceof Vec3 && b instanceof Vec3) {
        return new Vec3(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t);
    }
    if (a instanceof Vec2 && b instanceof Vec2) {
        return new Vec2(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t);
    }
    return a + (b - a) * t;
}
export function clamp(v, lo, hi) { return Math.max(lo, Math.min(hi, v)); }
export function smoothStep(min, max, val) {
    let x = clamp((val - min) / (max - min || 1), 0, 1);
    return x * x * (3 - 2 * x);
}
export const smoothstep = smoothStep;
export function fract(x) { return x - Math.floor(x); }
export function sign(x) { return x > 0 ? 1 : (x < 0 ? -1 : 0); }
export default { deg2rad, rad2deg, mix, clamp, smoothStep, smoothstep, fract, sign };
)JS";

constexpr const char* kModWEVector = R"JS(
export function angleVector2(angleDeg) {
    let r = angleDeg * Math.PI / 180.0;
    return new Vec2(Math.cos(r), Math.sin(r));
}
export function vectorAngle2(v) {
    return Math.atan2(v.y || 0, v.x || 0) * 180.0 / Math.PI;
}
export default { angleVector2, vectorAngle2 };
)JS";

constexpr const char* kBootstrapPrelude = R"JS(
class Vec2 {
    constructor(x = 0, y) {
        if (typeof x === 'string') {
            let p = x.trim().split(/\s+/).map(Number);
            this.x = p[0] || 0; this.y = p[1] || 0;
        } else if (x && typeof x === 'object') {
            this.x = Number(x.x ?? x[0] ?? 0);
            this.y = Number(x.y ?? x[1] ?? 0);
        } else {
            this.x = Number(x || 0);
            this.y = Number(y === undefined ? this.x : y);
        }
    }
    copy() { return new Vec2(this.x, this.y); }
    add(v) { return typeof v === 'number' ? new Vec2(this.x + v, this.y + v) : new Vec2(this.x + (v?.x||0), this.y + (v?.y||0)); }
    subtract(v) { return typeof v === 'number' ? new Vec2(this.x - v, this.y - v) : new Vec2(this.x - (v?.x||0), this.y - (v?.y||0)); }
    multiply(v) { return typeof v === 'number' ? new Vec2(this.x * v, this.y * v) : new Vec2(this.x * (v?.x??1), this.y * (v?.y??1)); }
    divide(v) { return typeof v === 'number' ? new Vec2(this.x / (v||1), this.y / (v||1)) : new Vec2(this.x / (v?.x||1), this.y / (v?.y||1)); }
    length() { return Math.hypot(this.x, this.y); }
    lengthSqr() { return this.x * this.x + this.y * this.y; }
    normalize() { let l = this.length() || 1; return new Vec2(this.x / l, this.y / l); }
    equals(v) { return v && this.x === v.x && this.y === v.y; }
    toString() { return `${this.x} ${this.y}`; }
}

class Vec3 {
    constructor(x = 0, y, z) {
        if (typeof x === 'string') {
            let p = x.trim().split(/\s+/).map(Number);
            this.x = p[0] || 0; this.y = p[1] || 0; this.z = p[2] || 0;
        } else if (x && typeof x === 'object') {
            this.x = Number(x.x ?? x.r ?? x[0] ?? 0);
            this.y = Number(x.y ?? x.g ?? x[1] ?? 0);
            this.z = Number(x.z ?? x.b ?? x[2] ?? 0);
        } else {
            this.x = Number(x || 0);
            this.y = Number(y === undefined ? this.x : y);
            this.z = Number(z === undefined ? (y === undefined ? this.x : 0) : z);
        }
    }
    get r() { return this.x; } set r(v) { this.x = Number(v || 0); }
    get g() { return this.y; } set g(v) { this.y = Number(v || 0); }
    get b() { return this.z; } set b(v) { this.z = Number(v || 0); }
    copy() { return new Vec3(this.x, this.y, this.z); }
    add(v) { return typeof v === 'number' ? new Vec3(this.x + v, this.y + v, this.z + v) : new Vec3(this.x + (v?.x||0), this.y + (v?.y||0), this.z + (v?.z||0)); }
    subtract(v) { return typeof v === 'number' ? new Vec3(this.x - v, this.y - v, this.z - v) : new Vec3(this.x - (v?.x||0), this.y - (v?.y||0), this.z - (v?.z||0)); }
    multiply(v) { return typeof v === 'number' ? new Vec3(this.x * v, this.y * v, this.z * v) : new Vec3(this.x * (v?.x??1), this.y * (v?.y??1), this.z * (v?.z??1)); }
    divide(v) { return typeof v === 'number' ? new Vec3(this.x / (v||1), this.y / (v||1), this.z / (v||1)) : new Vec3(this.x / (v?.x||1), this.y / (v?.y||1), this.z / (v?.z||1)); }
    dot(v) { return this.x * (v?.x||0) + this.y * (v?.y||0) + this.z * (v?.z||0); }
    cross(v) { return new Vec3(this.y*(v?.z||0)-this.z*(v?.y||0), this.z*(v?.x||0)-this.x*(v?.z||0), this.x*(v?.y||0)-this.y*(v?.x||0)); }
    length() { return Math.hypot(this.x, this.y, this.z); }
    lengthSqr() { return this.x * this.x + this.y * this.y + this.z * this.z; }
    normalize() { let l = this.length() || 1; return new Vec3(this.x / l, this.y / l, this.z / l); }
    min(v) { return typeof v === 'number' ? new Vec3(Math.min(this.x,v), Math.min(this.y,v), Math.min(this.z,v)) : new Vec3(Math.min(this.x,v.x), Math.min(this.y,v.y), Math.min(this.z,v.z)); }
    max(v) { return typeof v === 'number' ? new Vec3(Math.max(this.x,v), Math.max(this.y,v), Math.max(this.z,v)) : new Vec3(Math.max(this.x,v.x), Math.max(this.y,v.y), Math.max(this.z,v.z)); }
    equals(v) { return v && this.x === v.x && this.y === v.y && this.z === v.z; }
    toString() { return `${this.x} ${this.y} ${this.z}`; }
}

globalThis.Vec2 = Vec2;
globalThis.Vec3 = Vec3;
globalThis.shared = {};
globalThis.console = { log: ()=>{}, warn: ()=>{}, error: ()=>{}, info: ()=>{} };

globalThis.__audioBuffers = [];
globalThis.__currentScriptProps = null;
globalThis.__layers = [];
globalThis.__layersByName = Object.create(null);

globalThis.createScriptProperties = function() {
    const props = {};
    const applyOverride = (opt) => {
        if (!opt || !opt.name) return;
        let val = opt.value;
        const ov = globalThis.__currentScriptProps;
        if (ov && Object.prototype.hasOwnProperty.call(ov, opt.name)) {
            val = ov[opt.name];
        }
        props[opt.name] = val;
    };
    const builder = {
        addCheckbox(opt) { applyOverride(opt); return builder; },
        addSlider(opt) { applyOverride(opt); return builder; },
        addCombo(opt) { applyOverride(opt); return builder; },
        addText(opt) { applyOverride(opt); return builder; },
        addColor(opt) {
            applyOverride(opt);
            if (typeof props[opt?.name] === 'string') props[opt.name] = new Vec3(props[opt.name]);
            return builder;
        },
        addFile(opt) { applyOverride(opt); return builder; },
        addDirectory(opt) { applyOverride(opt); return builder; },
        finish() { return props; }
    };
    return builder;
};

globalThis.engine = {
    frametime: 1.0 / 30.0,
    runtime: 0.0,
    vram: 0,
    canvasSize: new Vec2(1920, 1080),
    screenResolution: new Vec2(1920, 1080),
    userProperties: {},
    AUDIO_RESOLUTION_16: 16,
    AUDIO_RESOLUTION_32: 32,
    AUDIO_RESOLUTION_64: 64,
    isRunningInEditor() { return false; },
    isDesktopDevice() { return true; },
    isWallpaper() { return true; },
    isScreensaver() { return false; },
    isPortrait() { return this.canvasSize.y > this.canvasSize.x; },
    setTimeout(fn) { return 0; },
    clearTimeout() {},
    registerAudioBuffers(res) {
        let n = (res === 64 || res === 32 || res === 16) ? res : 16;
        let buf = {
            resolution: n,
            left: new Float32Array(n),
            right: new Float32Array(n),
            average: new Float32Array(n)
        };
        globalThis.__audioBuffers.push(buf);
        return buf;
    },
    registerAsset() { return null; }
};

globalThis.input = {
    cursorWorldPosition: new Vec3(960, 540, 0),
    cursorScreenPosition: new Vec2(0.5, 0.5),
    cursorLeftDown: false
};

globalThis.thisScene = {
    getLayer(name) {
        return globalThis.__layersByName[name] || null;
    },
    getInitialLayerCount() {
        return globalThis.__layers.length;
    },
    enumerateLayers() {
        return globalThis.__layers.slice();
    },
    createLayer() { return null; },
    destroyLayer() {}
};

globalThis.__makeLayerProxy = function(init) {
    const layer = {
        id: init.id || 0,
        name: init.name || '',
        origin: new Vec3(init.origin || '0 0 0'),
        scale: new Vec3(init.scale || '1 1 1'),
        angles: new Vec3(init.angles || '0 0 0'),
        size: new Vec2(init.size || '256 64'),
        color: new Vec3(init.color || '1 1 1'),
        alpha: init.alpha !== undefined ? Number(init.alpha) : 1.0,
        visible: init.visible !== undefined ? Boolean(init.visible) : true,
        text: init.text || '',
        pointsize: init.pointsize !== undefined ? Number(init.pointsize) : 24.0,
        getParent() { return null; },
        getChildren() { return []; },
        getEffect(name) { return { name: name || '', visible: true }; },
        getEffectCount() { return 0; },
        getTextureAnimation() { return { play(){}, pause(){}, stop(){}, isPlaying(){ return true; }, getFrame(){ return 0; }, setFrame(){} }; },
        getVideoTexture() { return { play(){}, pause(){}, stop(){}, isPlaying(){ return false; }, getCurrentTime(){ return 0; }, setCurrentTime(){} }; },
        isAnimationPlaying() { return true; },
        play() {}, pause() {}, stop() {}
    };
    return layer;
};

globalThis.thisLayer = globalThis.__makeLayerProxy({});
globalThis.thisObject = globalThis.thisLayer;
)JS";

struct InterruptState {
    std::chrono::steady_clock::time_point deadline;
};

int JsInterruptHandler(JSContext* /*ctx*/, void* opaque) {
    auto* st = static_cast<InterruptState*>(opaque);
    if (! st) return 0;
    return std::chrono::steady_clock::now() > st->deadline ? 1 : 0;
}

JSModuleDef* BuiltinModuleLoader(JSContext* ctx, const char* moduleName, void* /*opaque*/) {
    const char* src = nullptr;
    if (std::strcmp(moduleName, "WEColor") == 0)
        src = kModWEColor;
    else if (std::strcmp(moduleName, "WEMath") == 0)
        src = kModWEMath;
    else if (std::strcmp(moduleName, "WEVector") == 0)
        src = kModWEVector;
    else
        return nullptr;

    JSValue val = JS_Eval(ctx, src, std::strlen(src), moduleName,
                          JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
    if (JS_IsException(val)) {
        JS_FreeValue(ctx, val);
        return nullptr;
    }
    auto* m = static_cast<JSModuleDef*>(JS_VALUE_GET_PTR(val));
    JS_FreeValue(ctx, val);
    return m;
}

const nlohmann::json* UnwrapJsField(const nlohmann::json& obj, const char* key) {
    if (! obj.is_object() || ! obj.contains(key)) return nullptr;
    const auto& v = obj.at(key);
    if (v.is_null()) return nullptr;
    if (v.is_object() && v.contains("value")) return &v.at("value");
    return &v;
}

std::string ReadStr(const nlohmann::json& obj, const char* key, const std::string& def) {
    const auto* v = UnwrapJsField(obj, key);
    if (! v || v->is_null()) return def;
    if (v->is_string()) return v->get<std::string>();
    return def;
}

float ReadFlt(const nlohmann::json& obj, const char* key, float def) {
    const auto* v = UnwrapJsField(obj, key);
    if (! v || v->is_null()) return def;
    if (v->is_number()) return static_cast<float>(v->get<double>());
    if (v->is_string()) {
        try { return std::stof(v->get<std::string>()); } catch (...) { return def; }
    }
    return def;
}

bool ReadBool(const nlohmann::json& obj, const char* key, bool def) {
    const auto* v = UnwrapJsField(obj, key);
    if (! v || v->is_null()) return def;
    if (v->is_boolean()) return v->get<bool>();
    if (v->is_number()) return v->get<double>() != 0.0;
    if (v->is_string()) {
        std::string s = ToLowerJs(v->get<std::string>());
        if (s == "true" || s == "1") return true;
        if (s == "false" || s == "0") return false;
    }
    return def;
}

std::array<float, 3> ParseVec3Str(const std::string& s, std::array<float, 3> def) {
    std::istringstream iss(s);
    for (int i = 0; i < 3; ++i) {
        float f = def[i];
        if (! (iss >> f)) break;
        def[i] = f;
    }
    return def;
}

JSValue JsonToJsValue(JSContext* ctx, const nlohmann::json& j) {
    if (j.is_null()) return JS_NULL;
    if (j.is_boolean()) return JS_NewBool(ctx, j.get<bool>() ? 1 : 0);
    if (j.is_number()) return JS_NewFloat64(ctx, j.get<double>());
    if (j.is_string()) {
        const auto& s = j.get_ref<const std::string&>();
        return JS_NewStringLen(ctx, s.data(), s.size());
    }
    if (j.is_array()) {
        JSValue arr = JS_NewArray(ctx);
        for (std::size_t i = 0; i < j.size(); ++i) {
            JS_SetPropertyUint32(ctx, arr, static_cast<uint32_t>(i), JsonToJsValue(ctx, j[i]));
        }
        return arr;
    }
    if (j.is_object()) {
        JSValue obj = JS_NewObject(ctx);
        for (auto it = j.begin(); it != j.end(); ++it) {
            JS_SetPropertyStr(ctx, obj, it.key().c_str(), JsonToJsValue(ctx, it.value()));
        }
        return obj;
    }
    return JS_UNDEFINED;
}

JSValue MakeJsVec3(JSContext* ctx, float x, float y, float z) {
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue ctor   = JS_GetPropertyStr(ctx, global, "Vec3");
    JS_FreeValue(ctx, global);
    JSValue argv[3] = { JS_NewFloat64(ctx, x), JS_NewFloat64(ctx, y), JS_NewFloat64(ctx, z) };
    JSValue res     = JS_CallConstructor(ctx, ctor, 3, argv);
    for (int i = 0; i < 3; ++i) JS_FreeValue(ctx, argv[i]);
    JS_FreeValue(ctx, ctor);
    return res;
}

JSValue MakeJsVec2(JSContext* ctx, float x, float y) {
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue ctor   = JS_GetPropertyStr(ctx, global, "Vec2");
    JS_FreeValue(ctx, global);
    JSValue argv[2] = { JS_NewFloat64(ctx, x), JS_NewFloat64(ctx, y) };
    JSValue res     = JS_CallConstructor(ctx, ctor, 2, argv);
    for (int i = 0; i < 2; ++i) JS_FreeValue(ctx, argv[i]);
    JS_FreeValue(ctx, ctor);
    return res;
}

bool ReadJsVec3(JSContext* ctx, JSValue val, std::array<float, 3>& out) {
    if (JS_IsString(val)) {
        const char* s = JS_ToCString(ctx, val);
        if (! s) return false;
        out = ParseVec3Str(s, out);
        JS_FreeCString(ctx, s);
        return true;
    }
    if (JS_IsObject(val)) {
        double  x = out[0], y = out[1], z = out[2];
        JSValue jx = JS_GetPropertyStr(ctx, val, "x");
        JSValue jy = JS_GetPropertyStr(ctx, val, "y");
        JSValue jz = JS_GetPropertyStr(ctx, val, "z");
        if (! JS_IsUndefined(jx)) JS_ToFloat64(ctx, &x, jx);
        if (! JS_IsUndefined(jy)) JS_ToFloat64(ctx, &y, jy);
        if (! JS_IsUndefined(jz)) JS_ToFloat64(ctx, &z, jz);
        JS_FreeValue(ctx, jx);
        JS_FreeValue(ctx, jy);
        JS_FreeValue(ctx, jz);
        if (std::isfinite(x)) out[0] = static_cast<float>(x);
        if (std::isfinite(y)) out[1] = static_cast<float>(y);
        if (std::isfinite(z)) out[2] = static_cast<float>(z);
        return true;
    }
    return false;
}

bool ReadJsVec2(JSContext* ctx, JSValue val, std::array<float, 2>& out) {
    if (JS_IsObject(val)) {
        double  x = out[0], y = out[1];
        JSValue jx = JS_GetPropertyStr(ctx, val, "x");
        JSValue jy = JS_GetPropertyStr(ctx, val, "y");
        if (! JS_IsUndefined(jx)) JS_ToFloat64(ctx, &x, jx);
        if (! JS_IsUndefined(jy)) JS_ToFloat64(ctx, &y, jy);
        JS_FreeValue(ctx, jx);
        JS_FreeValue(ctx, jy);
        if (std::isfinite(x)) out[0] = static_cast<float>(x);
        if (std::isfinite(y)) out[1] = static_cast<float>(y);
        return true;
    }
    return false;
}

void CollectSceneNodes(wallpaper::SceneNode* node,
                       std::unordered_map<int, wallpaper::SceneNode*>& byId,
                       std::unordered_map<std::string, wallpaper::SceneNode*>& byName) {
    if (! node) return;
    if (node->ID() != 0) byId[node->ID()] = node;
    if (! node->Name().empty()) byName[node->Name()] = node;
    for (auto& ch : node->GetChildren()) {
        CollectSceneNodes(ch.get(), byId, byName);
    }
}

enum class ScriptTargetKind
{
    Origin,
    Scale,
    Angles,
    Color,
    Alpha,
    Visible,
    Text,
    PointSize,
    Size,
    ShaderUniform,
};

struct LayerBinding {
    int                                               id { 0 };
    std::string                                       name;
    wallpaper::SceneNode*                             sceneNode { nullptr };
    wallpaper::SceneNode*                             transformNode { nullptr };
    std::shared_ptr<wallpaper::SceneImageEffectLayer> effectLayer;
    TextLayerState*                                   textLayer { nullptr };
    JSValue                                           jsProxy { JS_UNDEFINED };

    std::array<float, 3> savedScale { 1.0f, 1.0f, 1.0f };
    std::array<float, 3> color { 1.0f, 1.0f, 1.0f };
    float                alpha { 1.0f };
    bool                 visible { true };
    bool                 hasDynamicMutation { false };
};

struct ScriptInstance {
    std::size_t           layerIdx { 0 };
    ScriptTargetKind      kind { ScriptTargetKind::Text };
    std::string           uniformName;
    wallpaper::SceneNode* targetNode { nullptr };

    JSValue moduleNs { JS_UNDEFINED };
    JSValue fnInit { JS_UNDEFINED };
    JSValue fnUpdate { JS_UNDEFINED };
    JSValue fnApplyProps { JS_UNDEFINED };
    JSValue currentVal { JS_UNDEFINED };
};

} // namespace

struct SceneScriptEngine::Impl {
    JSRuntime*                  rt { nullptr };
    JSContext*                  ctx { nullptr };
    InterruptState              interrupt {};
    std::vector<LayerBinding>   layers;
    std::vector<ScriptInstance> scripts;

    ~Impl() { Shutdown(); }

    void ResetDeadline(int ms = 150) {
        interrupt.deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    }

    void Shutdown() {
        if (ctx) {
            for (auto& s : scripts) {
                JS_FreeValue(ctx, s.moduleNs);
                JS_FreeValue(ctx, s.fnInit);
                JS_FreeValue(ctx, s.fnUpdate);
                JS_FreeValue(ctx, s.fnApplyProps);
                JS_FreeValue(ctx, s.currentVal);
            }
            scripts.clear();
            for (auto& l : layers) {
                JS_FreeValue(ctx, l.jsProxy);
            }
            layers.clear();
            JS_FreeContext(ctx);
            ctx = nullptr;
        }
        if (rt) {
            JS_FreeRuntime(rt);
            rt = nullptr;
        }
    }

    bool InitVm(int orthoW, int orthoH, const std::string& userPropsJson) {
        Shutdown();

        rt = JS_NewRuntime();
        if (! rt) return false;
        JS_SetMemoryLimit(rt, 64 * 1024 * 1024);
        JS_SetMaxStackSize(rt, 1024 * 1024);
        JS_SetInterruptHandler(rt, JsInterruptHandler, &interrupt);
        JS_SetModuleLoaderFunc(rt, nullptr, BuiltinModuleLoader, nullptr);

        ctx = JS_NewContext(rt);
        if (! ctx) {
            Shutdown();
            return false;
        }

        ResetDeadline(300);
        JSValue r = JS_Eval(ctx, kBootstrapPrelude, std::strlen(kBootstrapPrelude), "<scenescript-prelude>", JS_EVAL_TYPE_GLOBAL);
        if (JS_IsException(r)) {
            LogException("bootstrap");
            JS_FreeValue(ctx, r);
            Shutdown();
            return false;
        }
        JS_FreeValue(ctx, r);

        // Set engine.canvasSize, engine.screenResolution, engine.userProperties
        JSValue global = JS_GetGlobalObject(ctx);
        JSValue engine = JS_GetPropertyStr(ctx, global, "engine");
        JS_SetPropertyStr(ctx, engine, "canvasSize", MakeJsVec2(ctx, static_cast<float>(orthoW), static_cast<float>(orthoH)));
        JS_SetPropertyStr(ctx, engine, "screenResolution", MakeJsVec2(ctx, static_cast<float>(orthoW), static_cast<float>(orthoH)));

        if (! userPropsJson.empty()) {
            try {
                auto jp = nlohmann::json::parse(userPropsJson);
                if (jp.is_object()) {
                    JS_SetPropertyStr(ctx, engine, "userProperties", JsonToJsValue(ctx, jp));
                }
            } catch (...) {}
        }
        JS_FreeValue(ctx, engine);
        JS_FreeValue(ctx, global);
        return true;
    }

    void LogException(const char* where) {
        if (! ctx) return;
        JSValue exc = JS_GetException(ctx);
        const char* msg = JS_ToCString(ctx, exc);
        if (msg) {
            LOG_ERROR("SceneScript(%s): %s", where, msg);
            JS_FreeCString(ctx, msg);
        }
        JS_FreeValue(ctx, exc);
    }

    void SetActiveLayerGlobal(std::size_t layerIdx) {
        if (layerIdx >= layers.size()) return;
        JSValue global = JS_GetGlobalObject(ctx);
        JS_SetPropertyStr(ctx, global, "thisLayer", JS_DupValue(ctx, layers[layerIdx].jsProxy));
        JS_SetPropertyStr(ctx, global, "thisObject", JS_DupValue(ctx, layers[layerIdx].jsProxy));
        JS_FreeValue(ctx, global);
    }

    void CompileAndAttachScript(std::size_t layerIdx, ScriptTargetKind kind,
                                const std::string& uniformName,
                                wallpaper::SceneNode* targetNode,
                                const nlohmann::json& propObj,
                                JSValue initialVal,
                                wallpaper::WPShaderValueUpdater* updater) {
        if (! propObj.is_object() || ! propObj.contains("script") || ! propObj["script"].is_string()) {
            JS_FreeValue(ctx, initialVal);
            return;
        }
        std::string code = propObj["script"].get<std::string>();
        // Skip empty or all-commented scripts quickly
        bool hasCode = false;
        for (char c : code) {
            if (! std::isspace(static_cast<unsigned char>(c))) { hasCode = true; break; }
        }
        if (! hasCode) {
            JS_FreeValue(ctx, initialVal);
            return;
        }

        // Unwrap scriptproperties (each entry may itself be {user, value} or scalar)
        nlohmann::json spUnwrapped = nlohmann::json::object();
        if (propObj.contains("scriptproperties") && propObj["scriptproperties"].is_object()) {
            for (auto it = propObj["scriptproperties"].begin(); it != propObj["scriptproperties"].end(); ++it) {
                if (it.value().is_object() && it.value().contains("value"))
                    spUnwrapped[it.key()] = it.value()["value"];
                else
                    spUnwrapped[it.key()] = it.value();
            }
        }

        SetActiveLayerGlobal(layerIdx);
        JSValue global = JS_GetGlobalObject(ctx);
        JS_SetPropertyStr(ctx, global, "__currentScriptProps", JsonToJsValue(ctx, spUnwrapped));
        JS_FreeValue(ctx, global);

        std::string modName = "script_" + std::to_string(scripts.size());
        ResetDeadline(250);
        JSValue compiled = JS_Eval(ctx, code.c_str(), code.size(), modName.c_str(),
                                   JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
        if (JS_IsException(compiled)) {
            LogException(modName.c_str());
            JS_FreeValue(ctx, compiled);
            JS_FreeValue(ctx, initialVal);
            return;
        }

        auto*   modPtr  = static_cast<JSModuleDef*>(JS_VALUE_GET_PTR(compiled));
        JSValue evalRes = JS_EvalFunction(ctx, compiled);
        if (JS_IsException(evalRes)) {
            LogException(modName.c_str());
            JS_FreeValue(ctx, evalRes);
            JS_FreeValue(ctx, initialVal);
            return;
        }
        JS_FreeValue(ctx, evalRes);

        JSValue ns           = JS_GetModuleNamespace(ctx, modPtr);
        JSValue fnInit       = JS_GetPropertyStr(ctx, ns, "init");
        JSValue fnUpdate     = JS_GetPropertyStr(ctx, ns, "update");
        JSValue fnApplyProps = JS_GetPropertyStr(ctx, ns, "applyUserProperties");

        bool hasAnyFn = JS_IsFunction(ctx, fnInit) || JS_IsFunction(ctx, fnUpdate) ||
                        JS_IsFunction(ctx, fnApplyProps);
        if (! hasAnyFn) {
            JS_FreeValue(ctx, ns);
            JS_FreeValue(ctx, fnInit);
            JS_FreeValue(ctx, fnUpdate);
            JS_FreeValue(ctx, fnApplyProps);
            JS_FreeValue(ctx, initialVal);
            return;
        }

        ScriptInstance inst {};
        inst.layerIdx     = layerIdx;
        inst.kind         = kind;
        inst.uniformName  = uniformName;
        inst.targetNode   = targetNode;
        inst.moduleNs     = ns;
        inst.fnInit       = JS_IsFunction(ctx, fnInit) ? fnInit : (JS_FreeValue(ctx, fnInit), JS_UNDEFINED);
        inst.fnUpdate     = JS_IsFunction(ctx, fnUpdate) ? fnUpdate : (JS_FreeValue(ctx, fnUpdate), JS_UNDEFINED);
        inst.fnApplyProps = JS_IsFunction(ctx, fnApplyProps) ? fnApplyProps : (JS_FreeValue(ctx, fnApplyProps), JS_UNDEFINED);
        inst.currentVal   = initialVal;

        // Run initial applyUserProperties(engine.userProperties) + init(initialVal)
        JSValue g2       = JS_GetGlobalObject(ctx);
        JSValue engine   = JS_GetPropertyStr(ctx, g2, "engine");
        JSValue allProps = JS_GetPropertyStr(ctx, engine, "userProperties");
        JS_FreeValue(ctx, engine);
        JS_FreeValue(ctx, g2);

        if (! JS_IsUndefined(inst.fnApplyProps)) {
            ResetDeadline(150);
            JSValue r = JS_Call(ctx, inst.fnApplyProps, JS_UNDEFINED, 1, &allProps);
            if (JS_IsException(r)) LogException("applyUserProperties");
            JS_FreeValue(ctx, r);
        }
        JS_FreeValue(ctx, allProps);

        if (! JS_IsUndefined(inst.fnInit)) {
            ResetDeadline(150);
            JSValue r = JS_Call(ctx, inst.fnInit, JS_UNDEFINED, 1, &inst.currentVal);
            if (JS_IsException(r)) {
                LogException("init");
                JS_FreeValue(ctx, r);
            } else if (! JS_IsUndefined(r) && ! JS_IsNull(r)) {
                JS_FreeValue(ctx, inst.currentVal);
                inst.currentVal = r;
                ApplyScriptReturnToTarget(inst, inst.currentVal, updater);
            } else {
                JS_FreeValue(ctx, r);
            }
        }

        scripts.push_back(std::move(inst));
        layers[layerIdx].hasDynamicMutation = true;
    }

    void PushAudioBuffers(const AudioSpectrumData& audio) {
        JSValue global  = JS_GetGlobalObject(ctx);
        JSValue arr     = JS_GetPropertyStr(ctx, global, "__audioBuffers");
        JS_FreeValue(ctx, global);
        if (! JS_IsObject(arr)) {
            JS_FreeValue(ctx, arr);
            return;
        }
        JSValue  lenVal = JS_GetPropertyStr(ctx, arr, "length");
        uint32_t count  = 0;
        JS_ToUint32(ctx, &count, lenVal);
        JS_FreeValue(ctx, lenVal);

        for (uint32_t i = 0; i < count; ++i) {
            JSValue  buf    = JS_GetPropertyUint32(ctx, arr, i);
            JSValue  resVal = JS_GetPropertyStr(ctx, buf, "resolution");
            uint32_t res    = 16;
            JS_ToUint32(ctx, &res, resVal);
            JS_FreeValue(ctx, resVal);

            const float* srcL = audio.left16.data();
            const float* srcR = audio.right16.data();
            if (res == 64) {
                srcL = audio.left64.data();
                srcR = audio.right64.data();
            } else if (res == 32) {
                srcL = audio.left32.data();
                srcR = audio.right32.data();
            } else {
                res = 16;
            }

            auto copyIntoTyped = [&](const char* prop, auto getter) {
                JSValue ta = JS_GetPropertyStr(ctx, buf, prop);
                size_t  byteOff = 0, byteLen = 0, bytesPerEl = 0;
                JSValue ab = JS_GetTypedArrayBuffer(ctx, ta, &byteOff, &byteLen, &bytesPerEl);
                if (! JS_IsException(ab) && bytesPerEl == sizeof(float)) {
                    size_t   abSize = 0;
                    uint8_t* ptr    = JS_GetArrayBuffer(ctx, &abSize, ab);
                    if (ptr && byteOff + res * sizeof(float) <= abSize) {
                        auto* dst = reinterpret_cast<float*>(ptr + byteOff);
                        for (uint32_t k = 0; k < res; ++k) dst[k] = getter(k);
                    }
                }
                JS_FreeValue(ctx, ab);
                JS_FreeValue(ctx, ta);
            };

            copyIntoTyped("left", [&](uint32_t k) { return srcL[k]; });
            copyIntoTyped("right", [&](uint32_t k) { return srcR[k]; });
            copyIntoTyped("average", [&](uint32_t k) { return 0.5f * (srcL[k] + srcR[k]); });
            JS_FreeValue(ctx, buf);
        }
        JS_FreeValue(ctx, arr);
    }

    void ApplyScriptReturnToTarget(ScriptInstance& inst, JSValue result,
                                   wallpaper::WPShaderValueUpdater* updater) {
        if (inst.layerIdx >= layers.size()) return;
        auto& layer = layers[inst.layerIdx];

        switch (inst.kind) {
        case ScriptTargetKind::Origin: {
            std::array<float, 3> v {};
            if (ReadJsVec3(ctx, result, v)) {
                JS_SetPropertyStr(ctx, layer.jsProxy, "origin", MakeJsVec3(ctx, v[0], v[1], v[2]));
            }
            break;
        }
        case ScriptTargetKind::Scale: {
            std::array<float, 3> v = layer.savedScale;
            if (JS_IsNumber(result)) {
                double d = 1.0;
                JS_ToFloat64(ctx, &d, result);
                if (std::isfinite(d)) v = { static_cast<float>(d), static_cast<float>(d), 1.0f };
                JS_SetPropertyStr(ctx, layer.jsProxy, "scale", MakeJsVec3(ctx, v[0], v[1], v[2]));
            } else if (ReadJsVec3(ctx, result, v)) {
                JS_SetPropertyStr(ctx, layer.jsProxy, "scale", MakeJsVec3(ctx, v[0], v[1], v[2]));
            }
            break;
        }
        case ScriptTargetKind::Angles: {
            std::array<float, 3> v {};
            if (ReadJsVec3(ctx, result, v)) {
                JS_SetPropertyStr(ctx, layer.jsProxy, "angles", MakeJsVec3(ctx, v[0], v[1], v[2]));
            }
            break;
        }
        case ScriptTargetKind::Color: {
            std::array<float, 3> v = layer.color;
            if (ReadJsVec3(ctx, result, v)) {
                JS_SetPropertyStr(ctx, layer.jsProxy, "color", MakeJsVec3(ctx, v[0], v[1], v[2]));
            }
            break;
        }
        case ScriptTargetKind::Alpha: {
            double d = layer.alpha;
            if (JS_ToFloat64(ctx, &d, result) == 0 && std::isfinite(d)) {
                JS_SetPropertyStr(ctx, layer.jsProxy, "alpha", JS_NewFloat64(ctx, std::clamp(d, 0.0, 1.0)));
            }
            break;
        }
        case ScriptTargetKind::Visible: {
            int b = JS_ToBool(ctx, result);
            if (b >= 0) {
                JS_SetPropertyStr(ctx, layer.jsProxy, "visible", JS_NewBool(ctx, b));
            }
            break;
        }
        case ScriptTargetKind::Text: {
            const char* s = JS_ToCString(ctx, result);
            if (s) {
                JS_SetPropertyStr(ctx, layer.jsProxy, "text", JS_NewString(ctx, s));
                JS_FreeCString(ctx, s);
            }
            break;
        }
        case ScriptTargetKind::PointSize: {
            double d = 24.0;
            if (JS_ToFloat64(ctx, &d, result) == 0 && std::isfinite(d) && d > 0.5) {
                JS_SetPropertyStr(ctx, layer.jsProxy, "pointsize", JS_NewFloat64(ctx, d));
            }
            break;
        }
        case ScriptTargetKind::Size: {
            std::array<float, 2> v { 256.0f, 64.0f };
            if (ReadJsVec2(ctx, result, v)) {
                JS_SetPropertyStr(ctx, layer.jsProxy, "size", MakeJsVec2(ctx, v[0], v[1]));
            }
            break;
        }
        case ScriptTargetKind::ShaderUniform: {
            wallpaper::SceneNode* target = inst.targetNode ? inst.targetNode : layer.sceneNode;
            if (! updater || ! target || inst.uniformName.empty()) break;
            if (JS_IsNumber(result)) {
                double d = 0.0;
                if (JS_ToFloat64(ctx, &d, result) == 0 && std::isfinite(d)) {
                    updater->SetNodeUniformOverride(target, inst.uniformName, static_cast<float>(d));
                }
            } else {
                std::array<float, 3> v { 1.0f, 1.0f, 1.0f };
                if (ReadJsVec3(ctx, result, v)) {
                    updater->SetNodeUniformOverride(target, inst.uniformName, wallpaper::ShaderValue(v));
                }
            }
            break;
        }
        }
    }

    void SyncLayerProxyToNative(LayerBinding& layer, wallpaper::WPShaderValueUpdater* updater) {
        if (! layer.hasDynamicMutation && ! layer.textLayer) return;

        // Read live JS proxy properties
        JSValue jVis = JS_GetPropertyStr(ctx, layer.jsProxy, "visible");
        bool    vis  = JS_ToBool(ctx, jVis) != 0;
        JS_FreeValue(ctx, jVis);

        JSValue jAlpha = JS_GetPropertyStr(ctx, layer.jsProxy, "alpha");
        double  a      = layer.alpha;
        JS_ToFloat64(ctx, &a, jAlpha);
        JS_FreeValue(ctx, jAlpha);
        if (! std::isfinite(a)) a = layer.alpha;
        layer.alpha   = static_cast<float>(std::clamp(a, 0.0, 1.0));
        layer.visible = vis;

        std::array<float, 3> origin {};
        std::array<float, 3> scale = layer.savedScale;
        std::array<float, 3> angles {};
        std::array<float, 3> color = layer.color;

        JSValue jOrg = JS_GetPropertyStr(ctx, layer.jsProxy, "origin");
        ReadJsVec3(ctx, jOrg, origin);
        JS_FreeValue(ctx, jOrg);

        JSValue jScl = JS_GetPropertyStr(ctx, layer.jsProxy, "scale");
        ReadJsVec3(ctx, jScl, scale);
        JS_FreeValue(ctx, jScl);
        layer.savedScale = scale;

        JSValue jAng = JS_GetPropertyStr(ctx, layer.jsProxy, "angles");
        ReadJsVec3(ctx, jAng, angles);
        JS_FreeValue(ctx, jAng);

        JSValue jCol = JS_GetPropertyStr(ctx, layer.jsProxy, "color");
        ReadJsVec3(ctx, jCol, color);
        JS_FreeValue(ctx, jCol);
        layer.color = color;

        if (layer.textLayer) {
            layer.textLayer->visible = vis;
            layer.textLayer->alpha   = layer.alpha;
            layer.textLayer->origin  = origin;
            layer.textLayer->scale   = scale;
            layer.textLayer->angles  = angles;
            layer.textLayer->color   = color;

            JSValue jText = JS_GetPropertyStr(ctx, layer.jsProxy, "text");
            if (JS_IsString(jText)) {
                const char* ts = JS_ToCString(ctx, jText);
                if (ts) {
                    layer.textLayer->text = ts;
                    JS_FreeCString(ctx, ts);
                }
            }
            JS_FreeValue(ctx, jText);

            JSValue jPs = JS_GetPropertyStr(ctx, layer.jsProxy, "pointsize");
            double  ps  = layer.textLayer->pointSize;
            if (JS_ToFloat64(ctx, &ps, jPs) == 0 && std::isfinite(ps) && ps > 0.5) {
                layer.textLayer->pointSize = static_cast<float>(ps);
            }
            JS_FreeValue(ctx, jPs);

            JSValue jSz = JS_GetPropertyStr(ctx, layer.jsProxy, "size");
            ReadJsVec2(ctx, jSz, layer.textLayer->size);
            JS_FreeValue(ctx, jSz);
        }

        wallpaper::SceneNode* xfNode = layer.transformNode ? layer.transformNode : layer.sceneNode;
        if (xfNode) {
            xfNode->SetTranslate(Eigen::Vector3f(origin[0], origin[1], origin[2]));
            xfNode->SetRotation(Eigen::Vector3f(angles[0], angles[1], angles[2]));
            if (vis) {
                xfNode->SetScale(Eigen::Vector3f(scale[0], scale[1], scale[2]));
            } else {
                xfNode->SetScale(Eigen::Vector3f(0.0f, 0.0f, 0.0f));
            }
        }
        if (layer.sceneNode && updater) {
            float effAlpha = vis ? layer.alpha : 0.0f;
            updater->SetNodeUniformOverride(layer.sceneNode, "g_UserAlpha", effAlpha);
            std::array<float, 4> c4 { color[0], color[1], color[2], effAlpha };
            updater->SetNodeUniformOverride(layer.sceneNode, "g_Color4", wallpaper::ShaderValue(c4));
        }
    }
};

SceneScriptEngine::SceneScriptEngine(): m_impl(std::make_unique<Impl>()) {}
SceneScriptEngine::~SceneScriptEngine() = default;

void SceneScriptEngine::Reset() {
    if (m_impl) m_impl->Shutdown();
}

std::size_t SceneScriptEngine::ActiveScriptCount() const {
    return m_impl ? m_impl->scripts.size() : 0;
}

bool SceneScriptEngine::Load(const std::string& effectiveSceneJson,
                             const std::string& userPropsJson, wallpaper::Scene& scene,
                             TextLayerRenderer& textRenderer) {
    Reset();
    if (effectiveSceneJson.empty()) return true;

    try {
        auto j = nlohmann::json::parse(effectiveSceneJson);
        if (! j.is_object() || ! j.contains("objects") || ! j["objects"].is_array())
            return true;

        // Quick scan: only spin up QuickJS VM if at least one script exists in scene.json.
        if (effectiveSceneJson.find("\"script\"") == std::string::npos) {
            return true;
        }

        if (! m_impl->InitVm(scene.ortho[0], scene.ortho[1], userPropsJson)) {
            return false;
        }

        std::unordered_map<int, wallpaper::SceneNode*>         nodesById;
        std::unordered_map<std::string, wallpaper::SceneNode*> nodesByName;
        CollectSceneNodes(scene.sceneGraph.get(), nodesById, nodesByName);

        JSValue global    = JS_GetGlobalObject(m_impl->ctx);
        JSValue makeProxy = JS_GetPropertyStr(m_impl->ctx, global, "__makeLayerProxy");
        JSValue layersArr = JS_GetPropertyStr(m_impl->ctx, global, "__layers");
        JSValue layersMap = JS_GetPropertyStr(m_impl->ctx, global, "__layersByName");

        const auto& objects = j["objects"];
        for (const auto& obj : objects) {
            if (! obj.is_object()) continue;

            LayerBinding lb {};
            lb.id      = obj.contains("id") && obj["id"].is_number_integer() ? obj["id"].get<int>() : 0;
            lb.name    = ReadStr(obj, "name", "");
            lb.alpha   = ReadFlt(obj, "alpha", 1.0f);
            lb.visible = ReadBool(obj, "visible", true);
            lb.color   = ParseVec3Str(ReadStr(obj, "color", "1 1 1"), { 1.0f, 1.0f, 1.0f });
            lb.savedScale = ParseVec3Str(ReadStr(obj, "scale", "1 1 1"), { 1.0f, 1.0f, 1.0f });

            if (obj.contains("text") && ! obj["text"].is_null()) {
                lb.textLayer = textRenderer.FindById(lb.id);
                if (! lb.textLayer && ! lb.name.empty()) lb.textLayer = textRenderer.FindByName(lb.name);
            } else {
                auto itId = (lb.id != 0) ? nodesById.find(lb.id) : nodesById.end();
                if (itId != nodesById.end())
                    lb.sceneNode = itId->second;
                else if (! lb.name.empty()) {
                    auto itNm = nodesByName.find(lb.name);
                    if (itNm != nodesByName.end()) lb.sceneNode = itNm->second;
                }
                lb.transformNode = lb.sceneNode;
                if (lb.sceneNode && ! lb.sceneNode->Camera().empty()) {
                    auto itCam = scene.cameras.find(lb.sceneNode->Camera());
                    if (itCam != scene.cameras.end() && itCam->second && itCam->second->HasImgEffect()) {
                        lb.effectLayer   = itCam->second->GetImgEffect();
                        lb.transformNode = &lb.effectLayer->FinalNode();
                    }
                }
            }

            nlohmann::json initObj = {
                { "id", lb.id },
                { "name", lb.name },
                { "origin", ReadStr(obj, "origin", "0 0 0") },
                { "scale", ReadStr(obj, "scale", "1 1 1") },
                { "angles", ReadStr(obj, "angles", "0 0 0") },
                { "size", ReadStr(obj, "size", "256 64") },
                { "color", ReadStr(obj, "color", "1 1 1") },
                { "alpha", lb.alpha },
                { "visible", lb.visible },
                { "text", ReadStr(obj, "text", "") },
                { "pointsize", ReadFlt(obj, "pointsize", 24.0f) },
            };

            JSValue arg   = JsonToJsValue(m_impl->ctx, initObj);
            JSValue proxy = JS_Call(m_impl->ctx, makeProxy, JS_UNDEFINED, 1, &arg);
            JS_FreeValue(m_impl->ctx, arg);
            lb.jsProxy = proxy;

            uint32_t idx = static_cast<uint32_t>(m_impl->layers.size());
            JS_SetPropertyUint32(m_impl->ctx, layersArr, idx, JS_DupValue(m_impl->ctx, proxy));
            if (! lb.name.empty()) {
                JS_SetPropertyStr(m_impl->ctx, layersMap, lb.name.c_str(), JS_DupValue(m_impl->ctx, proxy));
            }
            m_impl->layers.push_back(std::move(lb));
        }

        JS_FreeValue(m_impl->ctx, makeProxy);
        JS_FreeValue(m_impl->ctx, layersArr);
        JS_FreeValue(m_impl->ctx, layersMap);
        JS_FreeValue(m_impl->ctx, global);

        auto* updater =
            dynamic_cast<wallpaper::WPShaderValueUpdater*>(scene.shaderValueUpdater.get());

        // Compile scripts bound on each layer's properties & effects
        std::size_t layerIdx = 0;
        for (const auto& obj : objects) {
            if (! obj.is_object()) continue;
            if (layerIdx >= m_impl->layers.size()) break;

            auto& lb = m_impl->layers[layerIdx];
            auto  checkProp = [&](const char* key, ScriptTargetKind kind, JSValue initVal) {
                if (obj.contains(key) && obj[key].is_object() && obj[key].contains("script")) {
                    m_impl->CompileAndAttachScript(
                        layerIdx, kind, "", lb.sceneNode, obj[key], initVal, updater);
                } else {
                    JS_FreeValue(m_impl->ctx, initVal);
                }
            };

            checkProp("origin", ScriptTargetKind::Origin, JS_GetPropertyStr(m_impl->ctx, lb.jsProxy, "origin"));
            checkProp("scale", ScriptTargetKind::Scale, JS_GetPropertyStr(m_impl->ctx, lb.jsProxy, "scale"));
            checkProp("angles", ScriptTargetKind::Angles, JS_GetPropertyStr(m_impl->ctx, lb.jsProxy, "angles"));
            checkProp("color", ScriptTargetKind::Color, JS_GetPropertyStr(m_impl->ctx, lb.jsProxy, "color"));
            checkProp("alpha", ScriptTargetKind::Alpha, JS_GetPropertyStr(m_impl->ctx, lb.jsProxy, "alpha"));
            checkProp("visible", ScriptTargetKind::Visible, JS_GetPropertyStr(m_impl->ctx, lb.jsProxy, "visible"));
            checkProp("text", ScriptTargetKind::Text, JS_GetPropertyStr(m_impl->ctx, lb.jsProxy, "text"));
            checkProp("pointsize", ScriptTargetKind::PointSize, JS_GetPropertyStr(m_impl->ctx, lb.jsProxy, "pointsize"));
            checkProp("size", ScriptTargetKind::Size, JS_GetPropertyStr(m_impl->ctx, lb.jsProxy, "size"));

            // Also scan effects[].passes[].constantshadervalues.* for scripted uniforms (e.g. rainbow tint color)
            if (obj.contains("effects") && obj["effects"].is_array()) {
                std::size_t visibleEffIdx = 0;
                for (const auto& eff : obj["effects"]) {
                    if (! eff.is_object()) continue;
                    if (! ReadBool(eff, "visible", true)) continue;
                    std::size_t curEffIdx = visibleEffIdx++;
                    if (! eff.contains("passes") || ! eff["passes"].is_array()) continue;

                    std::shared_ptr<wallpaper::SceneImageEffect> imgEff;
                    if (lb.effectLayer && curEffIdx < lb.effectLayer->EffectCount()) {
                        imgEff = lb.effectLayer->GetEffect(curEffIdx);
                    }

                    std::size_t passIdx = 0;
                    for (const auto& pass : eff["passes"]) {
                        std::size_t curPassIdx = passIdx++;
                        if (! pass.is_object() || ! pass.contains("constantshadervalues") ||
                            ! pass["constantshadervalues"].is_object())
                            continue;

                        wallpaper::SceneNode* effPassNode = lb.sceneNode;
                        if (imgEff) {
                            std::size_t ni = 0;
                            for (const auto& en : imgEff->nodes) {
                                if (ni == curPassIdx) {
                                    effPassNode = en.sceneNode.get();
                                    break;
                                }
                                ++ni;
                            }
                        }

                        const auto& csv = pass["constantshadervalues"];
                        for (auto it = csv.begin(); it != csv.end(); ++it) {
                            if (! it.value().is_object() || ! it.value().contains("script")) continue;
                            JSValue initU = JS_UNDEFINED;
                            if (it.value().contains("value")) {
                                const auto& v = it.value()["value"];
                                if (v.is_string()) {
                                    auto vec = ParseVec3Str(v.get<std::string>(), { 1.0f, 1.0f, 1.0f });
                                    initU    = MakeJsVec3(m_impl->ctx, vec[0], vec[1], vec[2]);
                                } else {
                                    initU = JsonToJsValue(m_impl->ctx, v);
                                }
                            }
                            m_impl->CompileAndAttachScript(layerIdx,
                                                           ScriptTargetKind::ShaderUniform,
                                                           it.key(),
                                                           effPassNode,
                                                           it.value(),
                                                           initU,
                                                           updater);
                        }
                    }
                }
            }

            ++layerIdx;
        }

        // Sync initial init()/applyUserProperties() side-effects immediately so frame 0 is accurate
        for (auto& layer : m_impl->layers) {
            if (layer.hasDynamicMutation) {
                m_impl->SyncLayerProxyToNative(layer, updater);
            }
        }

        LOG_INFO("SceneScriptEngine loaded %zu active scripts across %zu layers",
                 m_impl->scripts.size(),
                 m_impl->layers.size());
        return true;
    } catch (const std::exception& ex) {
        LOG_ERROR("SceneScriptEngine::Load warning: %s", ex.what());
        return false;
    }
}

void SceneScriptEngine::Update(double deltaSec, double elapsedSec, const AudioSpectrumData& audio,
                               wallpaper::Scene& scene) {
    if (! m_impl || ! m_impl->ctx || m_impl->scripts.empty()) return;

    JSContext* ctx = m_impl->ctx;
    JSValue    g   = JS_GetGlobalObject(ctx);
    JSValue    eng = JS_GetPropertyStr(ctx, g, "engine");
    JS_SetPropertyStr(ctx, eng, "frametime", JS_NewFloat64(ctx, deltaSec));
    JS_SetPropertyStr(ctx, eng, "runtime", JS_NewFloat64(ctx, elapsedSec));
    JS_FreeValue(ctx, eng);
    JS_FreeValue(ctx, g);

    m_impl->PushAudioBuffers(audio);

    auto* updater =
        dynamic_cast<wallpaper::WPShaderValueUpdater*>(scene.shaderValueUpdater.get());

    for (auto& inst : m_impl->scripts) {
        if (JS_IsUndefined(inst.fnUpdate)) continue;
        m_impl->SetActiveLayerGlobal(inst.layerIdx);
        m_impl->ResetDeadline(100);

        JSValue res = JS_Call(ctx, inst.fnUpdate, JS_UNDEFINED, 1, &inst.currentVal);
        if (JS_IsException(res)) {
            m_impl->LogException("update");
            JS_FreeValue(ctx, res);
            continue;
        }
        if (! JS_IsUndefined(res) && ! JS_IsNull(res)) {
            JS_FreeValue(ctx, inst.currentVal);
            inst.currentVal = JS_DupValue(ctx, res);
            m_impl->ApplyScriptReturnToTarget(inst, res, updater);
        }
        JS_FreeValue(ctx, res);
    }

    for (auto& layer : m_impl->layers) {
        if (layer.hasDynamicMutation) {
            m_impl->SyncLayerProxyToNative(layer, updater);
        }
    }
}

void SceneScriptEngine::ApplyUserProperties(const std::string& changedPropsJson) {
    if (! m_impl || ! m_impl->ctx || m_impl->scripts.empty() || changedPropsJson.empty()) return;
    try {
        auto jp = nlohmann::json::parse(changedPropsJson);
        if (! jp.is_object() || jp.empty()) return;

        JSContext* ctx     = m_impl->ctx;
        JSValue    changed = JsonToJsValue(ctx, jp);

        // Merge into engine.userProperties
        JSValue g        = JS_GetGlobalObject(ctx);
        JSValue eng      = JS_GetPropertyStr(ctx, g, "engine");
        JSValue allProps = JS_GetPropertyStr(ctx, eng, "userProperties");
        for (auto it = jp.begin(); it != jp.end(); ++it) {
            JS_SetPropertyStr(ctx, allProps, it.key().c_str(), JsonToJsValue(ctx, it.value()));
        }
        JS_FreeValue(ctx, allProps);
        JS_FreeValue(ctx, eng);
        JS_FreeValue(ctx, g);

        for (auto& inst : m_impl->scripts) {
            if (JS_IsUndefined(inst.fnApplyProps)) continue;
            m_impl->SetActiveLayerGlobal(inst.layerIdx);
            m_impl->ResetDeadline(150);
            JSValue r = JS_Call(ctx, inst.fnApplyProps, JS_UNDEFINED, 1, &changed);
            if (JS_IsException(r)) m_impl->LogException("applyUserProperties");
            JS_FreeValue(ctx, r);
        }
        JS_FreeValue(ctx, changed);
    } catch (...) {}
}

} // namespace lcdfusion_wpe

#include "WebWallpaperWin32.hpp"
#include "AudioSpectrumWin32.hpp"
#include "Utils/Logging.h"

#include <windows.h>
#include <dwmapi.h>
#include <objbase.h>
#include <shlwapi.h>
#include <WebView2.h>

#include <nlohmann/json.hpp>
#include <stb_image.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>

namespace lcdfusion_wpe
{

namespace
{

constexpr GUID kIidEnvOptions = {
    0x2fde08a8, 0x1e9a, 0x4766, { 0x8c, 0x05, 0x95, 0xa9, 0xce, 0xb9, 0xd1, 0xc5 }
};
constexpr GUID kIidWebView2_3 = {
    0xA0D6DF20, 0x3B92, 0x416D, { 0xAA, 0x0C, 0x43, 0x7A, 0x9C, 0x72, 0x78, 0x57 }
};
constexpr GUID kIidWebView2_8 = {
    0xE9632730, 0x6E1E, 0x43AB, { 0xB7, 0xB8, 0x7B, 0x2C, 0x9E, 0x62, 0xE0, 0x94 }
};
constexpr GUID kIidController3 = {
    0xf9614724, 0x5d2b, 0x41dc, { 0xae, 0xf7, 0x73, 0xd6, 0x2b, 0x51, 0x54, 0x3b }
};
constexpr GUID kIidEnvCompleted = {
    0x4e8a3389, 0xc9d8, 0x4bd2, { 0xb6, 0xb5, 0x12, 0x4f, 0xee, 0x6c, 0xc1, 0x4d }
};
constexpr GUID kIidCtrlCompleted = {
    0x6c4819f3, 0xc9b7, 0x4260, { 0x81, 0x27, 0xc9, 0xf5, 0xbd, 0xe7, 0xf6, 0x8c }
};
constexpr GUID kIidCdpEvent = {
    0xe2fda4be, 0x5456, 0x406c, { 0xa2, 0x61, 0x3d, 0x45, 0x21, 0x38, 0x36, 0x2c }
};
constexpr GUID kIidCdpMethod = {
    0x5c4889f0, 0x5ef6, 0x4c5a, { 0x95, 0x2c, 0xd8, 0xf1, 0xb9, 0x2d, 0x05, 0x74 }
};
constexpr GUID kIidNavCompleted = {
    0xd33a35bf, 0x1c49, 0x4f98, { 0x93, 0xab, 0x00, 0x6e, 0x05, 0x33, 0xfe, 0x1c }
};
constexpr GUID kIidCapturePreview = {
    0x697e05e9, 0x3d8f, 0x45fa, { 0x96, 0xf4, 0x8f, 0xfe, 0x1e, 0xde, 0xda, 0xf5 }
};

constexpr UINT WM_WPE_RESIZE     = WM_APP + 1;
constexpr UINT WM_WPE_PROPS      = WM_APP + 2;
constexpr UINT WM_WPE_PAUSED     = WM_APP + 3;
constexpr UINT WM_WPE_AUDIO      = WM_APP + 4;
constexpr UINT WM_WPE_CLOSE      = WM_APP + 5;
constexpr UINT WM_WPE_POLL_FALLBACK = WM_APP + 6;
constexpr UINT WM_WPE_SENSORS    = WM_APP + 7;

std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return {};
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string WideToUtf8(const wchar_t* w) {
    if (! w || ! *w) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string s(static_cast<std::size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

LPWSTR DupCoTaskStr(const std::wstring& s) {
    const std::size_t bytes = (s.size() + 1) * sizeof(wchar_t);
    auto*             p     = static_cast<LPWSTR>(CoTaskMemAlloc(bytes));
    if (! p) return nullptr;
    std::memcpy(p, s.c_str(), bytes);
    return p;
}

std::string UrlEncodeRelPath(const std::string& rel) {
    std::string out;
    out.reserve(rel.size() * 2);
    for (unsigned char c : rel) {
        if (c == '\\') {
            out.push_back('/');
        } else if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                   c == '-' || c == '_' || c == '.' || c == '~' || c == '/') {
            out.push_back(static_cast<char>(c));
        } else {
            static const char kHex[] = "0123456789ABCDEF";
            out.push_back('%');
            out.push_back(kHex[c >> 4]);
            out.push_back(kHex[c & 0x0F]);
        }
    }
    while (! out.empty() && out.front() == '/') out.erase(out.begin());
    return out;
}

// Fast standard Base64 decoder (ignores whitespace).
std::vector<uint8_t> Base64Decode(const std::string& b64) {
    static const int8_t kDec[256] = {
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,-1,63,
        52,53,54,55,56,57,58,59,60,61,-1,-1,-1,-1,-1,-1,
        -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,
        15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,-1,
        -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,
        41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1
    };
    std::vector<uint8_t> out;
    out.reserve((b64.size() * 3) / 4);
    int val  = 0;
    int valb = -8;
    for (unsigned char c : b64) {
        if (c == '=') break;
        int8_t d = kDec[c];
        if (d == -1) continue;
        val = (val << 6) + d;
        valb += 6;
        if (valb >= 0) {
            out.push_back(static_cast<uint8_t>((val >> valb) & 0xFF));
            valb -= 8;
        }
    }
    return out;
}

void RgbaToBgraWithFit(const uint8_t* srcRgba, uint32_t srcW, uint32_t srcH,
                       uint8_t* dstBgra, uint32_t dstW, uint32_t dstH, int fitMode) {
    if (fitMode == 1 && (srcW * dstH != srcH * dstW)) {
        // Fit (letterbox / pillarbox with black bars)
        std::memset(dstBgra, 0, static_cast<std::size_t>(dstW) * dstH * 4u);
        for (std::size_t i = 3; i < static_cast<std::size_t>(dstW) * dstH * 4u; i += 4) dstBgra[i] = 255;

        const double srcAspect = static_cast<double>(srcW) / static_cast<double>(srcH);
        const double dstAspect = static_cast<double>(dstW) / static_cast<double>(dstH);
        uint32_t     subW = dstW, subH = dstH, offX = 0, offY = 0;
        if (srcAspect > dstAspect) {
            subH = std::max<uint32_t>(1, std::min(dstH, static_cast<uint32_t>(std::lround(dstW / srcAspect))));
            offY = (dstH - subH) / 2u;
        } else {
            subW = std::max<uint32_t>(1, std::min(dstW, static_cast<uint32_t>(std::lround(dstH * srcAspect))));
            offX = (dstW - subW) / 2u;
        }
        const double scaleX = static_cast<double>(srcW) / static_cast<double>(subW);
        const double scaleY = static_cast<double>(srcH) / static_cast<double>(subH);
        for (uint32_t y = 0; y < subH; ++y) {
            double   sy = (static_cast<double>(y) + 0.5) * scaleY - 0.5;
            int      y0 = std::clamp(static_cast<int>(std::floor(sy)), 0, static_cast<int>(srcH) - 1);
            int      y1 = std::min(y0 + 1, static_cast<int>(srcH) - 1);
            float    fy = static_cast<float>(std::clamp(sy - y0, 0.0, 1.0));
            uint8_t* row = dstBgra + (static_cast<std::size_t>(offY + y) * dstW + offX) * 4u;
            const uint8_t* sRow0 = srcRgba + static_cast<std::size_t>(y0) * srcW * 4u;
            const uint8_t* sRow1 = srcRgba + static_cast<std::size_t>(y1) * srcW * 4u;
            for (uint32_t x = 0; x < subW; ++x) {
                double sx = (static_cast<double>(x) + 0.5) * scaleX - 0.5;
                int    x0 = std::clamp(static_cast<int>(std::floor(sx)), 0, static_cast<int>(srcW) - 1);
                int    x1 = std::min(x0 + 1, static_cast<int>(srcW) - 1);
                float  fx = static_cast<float>(std::clamp(sx - x0, 0.0, 1.0));
                const uint8_t* p00 = sRow0 + x0 * 4;
                const uint8_t* p10 = sRow0 + x1 * 4;
                const uint8_t* p01 = sRow1 + x0 * 4;
                const uint8_t* p11 = sRow1 + x1 * 4;
                auto lerp = [&](int ch) -> uint8_t {
                    float top = p00[ch] + (p10[ch] - p00[ch]) * fx;
                    float bot = p01[ch] + (p11[ch] - p01[ch]) * fx;
                    return static_cast<uint8_t>(std::clamp(std::lround(top + (bot - top) * fy), 0L, 255L));
                };
                row[x * 4 + 0] = lerp(2);
                row[x * 4 + 1] = lerp(1);
                row[x * 4 + 2] = lerp(0);
                row[x * 4 + 3] = 255;
            }
        }
        return;
    }

    if (srcW == dstW && srcH == dstH) {
        const std::size_t pixels = static_cast<std::size_t>(dstW) * dstH;
        for (std::size_t i = 0; i < pixels; ++i) {
            dstBgra[i * 4 + 0] = srcRgba[i * 4 + 2];
            dstBgra[i * 4 + 1] = srcRgba[i * 4 + 1];
            dstBgra[i * 4 + 2] = srcRgba[i * 4 + 0];
            dstBgra[i * 4 + 3] = 255;
        }
        return;
    }

    // Bilinear resize (source already rendered at viewport aspect ratio)
    const double scaleX = static_cast<double>(srcW) / static_cast<double>(dstW);
    const double scaleY = static_cast<double>(srcH) / static_cast<double>(dstH);
    for (uint32_t y = 0; y < dstH; ++y) {
        double   sy = (static_cast<double>(y) + 0.5) * scaleY - 0.5;
        int      y0 = std::clamp(static_cast<int>(std::floor(sy)), 0, static_cast<int>(srcH) - 1);
        int      y1 = std::min(y0 + 1, static_cast<int>(srcH) - 1);
        float    fy = static_cast<float>(std::clamp(sy - y0, 0.0, 1.0));
        uint8_t* row = dstBgra + static_cast<std::size_t>(y) * dstW * 4u;
        const uint8_t* sRow0 = srcRgba + static_cast<std::size_t>(y0) * srcW * 4u;
        const uint8_t* sRow1 = srcRgba + static_cast<std::size_t>(y1) * srcW * 4u;

        for (uint32_t x = 0; x < dstW; ++x) {
            double sx = (static_cast<double>(x) + 0.5) * scaleX - 0.5;
            int    x0 = std::clamp(static_cast<int>(std::floor(sx)), 0, static_cast<int>(srcW) - 1);
            int    x1 = std::min(x0 + 1, static_cast<int>(srcW) - 1);
            float  fx = static_cast<float>(std::clamp(sx - x0, 0.0, 1.0));

            const uint8_t* p00 = sRow0 + x0 * 4;
            const uint8_t* p10 = sRow0 + x1 * 4;
            const uint8_t* p01 = sRow1 + x0 * 4;
            const uint8_t* p11 = sRow1 + x1 * 4;

            auto lerp = [&](int ch) -> uint8_t {
                float top = p00[ch] + (p10[ch] - p00[ch]) * fx;
                float bot = p01[ch] + (p11[ch] - p01[ch]) * fx;
                return static_cast<uint8_t>(std::clamp(std::lround(top + (bot - top) * fy), 0L, 255L));
            };
            row[x * 4 + 0] = lerp(2); // B
            row[x * 4 + 1] = lerp(1); // G
            row[x * 4 + 2] = lerp(0); // R
            row[x * 4 + 3] = 255;     // A
        }
    }
}

// Wallpaper Engine Web API shim injected before page scripts execute.
constexpr const wchar_t* kWebWallpaperBridgeScript = LR"JS(
(function() {
    if (window.__wpeBridgeInstalled) return;
    window.__wpeBridgeInstalled = true;

    let _listener = null;
    let _audioListener = null;
    let _propsMap = Object.create(null);
    let _initialFlushed = false;
    let _pendingTimer = 0;

    function wrapProps(raw) {
        const out = {};
        if (!raw || typeof raw !== 'object') return out;
        for (const k of Object.keys(raw)) {
            const v = raw[k];
            out[k] = (v && typeof v === 'object' && Object.prototype.hasOwnProperty.call(v, 'value'))
                ? v
                : { value: v };
        }
        return out;
    }

    function flushToListener(subset) {
        if (!_listener || typeof _listener.applyUserProperties !== 'function') return;
        const payload = subset ? wrapProps(subset) : wrapProps(_propsMap);
        if (Object.keys(payload).length === 0) return;
        try {
            _listener.applyUserProperties(payload);
        } catch (e) {}
    }

    function scheduleInitialFlush() {
        if (_pendingTimer) clearTimeout(_pendingTimer);
        _pendingTimer = setTimeout(function() {
            _pendingTimer = 0;
            if (!_listener) return;
            _initialFlushed = true;
            flushToListener(null);
            if (typeof _listener.applyGeneralProperties === 'function') {
                try { _listener.applyGeneralProperties({ fps: 30 }); } catch (e) {}
            }
        }, 20);
    }

    Object.defineProperty(window, 'wallpaperPropertyListener', {
        configurable: true,
        enumerable: true,
        get: function() { return _listener; },
        set: function(val) {
            _listener = val;
            _initialFlushed = false;
            if (val) scheduleInitialFlush();
        }
    });

    window.wallpaperRegisterAudioListener = function(cb) {
        _audioListener = (typeof cb === 'function') ? cb : null;
    };

    window.__wpeApplyProps = function(raw) {
        if (!raw || typeof raw !== 'object') return;
        for (const k of Object.keys(raw)) {
            _propsMap[k] = raw[k];
        }
        if (_listener) {
            if (_initialFlushed) {
                flushToListener(raw);
            } else {
                scheduleInitialFlush();
            }
        }
    };

    window.__wpeSetPaused = function(paused) {
        if (_listener && typeof _listener.setPaused === 'function') {
            try { _listener.setPaused(!!paused); } catch (e) {}
        }
    };

    window.__wpePushAudio = function(arr128) {
        if (_audioListener && Array.isArray(arr128)) {
            try { _audioListener(arr128); } catch (e) {}
        }
    };

    window.__wpeHasAudioListener = function() {
        return _audioListener !== null ? 1 : 0;
    };

    let _sensorListeners = [];
    let _latestSensors = null;
    window.lcdFusionSensors = null;

    window.lcdFusionRegisterSensorListener = function(cb) {
        if (typeof cb !== 'function') return;
        _sensorListeners.push(cb);
        if (_latestSensors) {
            try { cb(_latestSensors); } catch (e) {}
        }
    };

    window.__wpePushSensors = function(data) {
        if (!data || typeof data !== 'object') return;
        _latestSensors = data;
        window.lcdFusionSensors = data;
        for (let i = 0; i < _sensorListeners.length; i++) {
            try { _sensorListeners[i](data); } catch (e) {}
        }
    };

    // Suppress blocking modal alerts from Web wallpapers in headless mode.
    window.alert = function() {};
    window.confirm = function() { return false; };
    window.prompt = function() { return null; };
})();
)JS";

// Minimal COM implementation of ICoreWebView2EnvironmentOptions.
class SimpleEnvOptions final : public ICoreWebView2EnvironmentOptions {
public:
    explicit SimpleEnvOptions(std::wstring args): m_args(std::move(args)) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (! ppv) return E_POINTER;
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, kIidEnvOptions)) {
            *ppv = static_cast<ICoreWebView2EnvironmentOptions*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++m_ref; }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG r = --m_ref;
        if (r == 0) delete this;
        return r;
    }

    HRESULT STDMETHODCALLTYPE get_AdditionalBrowserArguments(LPWSTR* value) override {
        if (! value) return E_POINTER;
        *value = DupCoTaskStr(m_args);
        return *value ? S_OK : E_OUTOFMEMORY;
    }
    HRESULT STDMETHODCALLTYPE put_AdditionalBrowserArguments(LPCWSTR value) override {
        m_args = value ? value : L"";
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_Language(LPWSTR* value) override {
        if (! value) return E_POINTER;
        *value = m_lang.empty() ? nullptr : DupCoTaskStr(m_lang);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE put_Language(LPCWSTR value) override {
        m_lang = value ? value : L"";
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_TargetCompatibleBrowserVersion(LPWSTR* value) override {
        if (! value) return E_POINTER;
        *value = m_targetVer.empty() ? nullptr : DupCoTaskStr(m_targetVer);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE put_TargetCompatibleBrowserVersion(LPCWSTR value) override {
        m_targetVer = value ? value : L"";
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_AllowSingleSignOnUsingOSPrimaryAccount(BOOL* allow) override {
        if (! allow) return E_POINTER;
        *allow = m_sso;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE put_AllowSingleSignOnUsingOSPrimaryAccount(BOOL allow) override {
        m_sso = allow;
        return S_OK;
    }

private:
    std::atomic<ULONG> m_ref { 1 };
    std::wstring       m_args;
    std::wstring       m_lang;
    std::wstring       m_targetVer { L"110.0.1587.41" };
    BOOL               m_sso { FALSE };
};

template <typename Iface, const GUID* Iid, typename... Args>
class ComHandler final : public Iface {
public:
    using Fn = std::function<HRESULT(Args...)>;
    explicit ComHandler(Fn fn): m_fn(std::move(fn)) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (! ppv) return E_POINTER;
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, *Iid)) {
            *ppv = static_cast<Iface*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++m_ref; }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG r = --m_ref;
        if (r == 0) delete this;
        return r;
    }
    HRESULT STDMETHODCALLTYPE Invoke(Args... args) override {
        return m_fn ? m_fn(args...) : S_OK;
    }

private:
    std::atomic<ULONG> m_ref { 1 };
    Fn                 m_fn;
};

// Compare dotted numeric version strings (e.g., "157.0.4341.60" > "130.0.1.0").
bool IsVersionNewer(const std::wstring& a, const std::wstring& b) {
    auto parse = [](const std::wstring& s) {
        std::vector<int> v;
        std::wstringstream ss(s);
        std::wstring       tok;
        while (std::getline(ss, tok, L'.')) {
            try { v.push_back(std::stoi(tok)); } catch (...) { v.push_back(0); }
        }
        return v;
    };
    auto va = parse(a);
    auto vb = parse(b);
    std::size_t n = std::max(va.size(), vb.size());
    va.resize(n, 0);
    vb.resize(n, 0);
    return va > vb;
}

std::wstring ReadRegString(HKEY root, const wchar_t* subkey, const wchar_t* valueName, REGSAM wowFlag) {
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(root, subkey, 0, KEY_READ | wowFlag, &hKey) != ERROR_SUCCESS) return {};
    wchar_t buf[1024] = {};
    DWORD   cb        = sizeof(buf);
    DWORD   type      = 0;
    std::wstring result;
    if (RegQueryValueExW(hKey, valueName, nullptr, &type, reinterpret_cast<LPBYTE>(buf), &cb) == ERROR_SUCCESS &&
        (type == REG_SZ || type == REG_EXPAND_SZ)) {
        result = buf;
    }
    RegCloseKey(hKey);
    return result;
}

std::wstring FindNewestEmbeddedWebViewDllInRoot(const std::filesystem::path& appRoot) {
    std::error_code ec;
    if (! std::filesystem::exists(appRoot, ec)) return {};

    std::wstring bestVer;
    std::wstring bestDll;
    for (const auto& entry : std::filesystem::directory_iterator(appRoot, ec)) {
        if (! entry.is_directory(ec)) continue;
        std::wstring dirName = entry.path().filename().wstring();
        if (dirName.empty() || dirName[0] < L'0' || dirName[0] > L'9') continue;
        auto candidate = entry.path() / L"EBWebView" / L"x64" / L"EmbeddedBrowserWebView.dll";
        if (std::filesystem::exists(candidate, ec)) {
            if (bestVer.empty() || IsVersionNewer(dirName, bestVer)) {
                bestVer = dirName;
                bestDll = candidate.wstring();
            }
        }
    }
    return bestDll;
}

std::wstring LocateEmbeddedWebView2Dll() {
    // 1. Check standard Evergreen WebView2 registry entries (HKLM & HKCU, WOW6432 & native)
    constexpr const wchar_t* kClientKey =
        L"SOFTWARE\\Microsoft\\EdgeUpdate\\Clients\\{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}";
    const HKEY       kRoots[] = { HKEY_LOCAL_MACHINE, HKEY_CURRENT_USER };
    const REGSAM     kWows[]  = { KEY_WOW64_32KEY, KEY_WOW64_64KEY };

    for (HKEY root : kRoots) {
        for (REGSAM wow : kWows) {
            std::wstring pv = ReadRegString(root, kClientKey, L"pv", wow);
            if (pv.empty() || pv == L"0.0.0.0") continue;
            std::wstring loc = ReadRegString(root, kClientKey, L"location", wow);
            if (! loc.empty()) {
                auto candidate = std::filesystem::path(loc) / pv / L"EBWebView" / L"x64" / L"EmbeddedBrowserWebView.dll";
                std::error_code ec;
                if (std::filesystem::exists(candidate, ec)) return candidate.wstring();
            }
        }
    }

    // 2. Scan well-known EdgeWebView & Edge installation directories
    std::vector<std::filesystem::path> roots;
    auto pushEnvRoot = [&](const wchar_t* envName, const wchar_t* suffix) {
        wchar_t buf[1024] = {};
        DWORD   n         = GetEnvironmentVariableW(envName, buf, 1024);
        if (n > 0 && n < 1024) {
            roots.push_back(std::filesystem::path(buf) / suffix);
        }
    };
    pushEnvRoot(L"ProgramFiles(x86)", L"Microsoft\\EdgeWebView\\Application");
    pushEnvRoot(L"ProgramFiles", L"Microsoft\\EdgeWebView\\Application");
    pushEnvRoot(L"LOCALAPPDATA", L"Microsoft\\EdgeWebView\\Application");
    pushEnvRoot(L"ProgramFiles(x86)", L"Microsoft\\Edge\\Application");
    pushEnvRoot(L"ProgramFiles", L"Microsoft\\Edge\\Application");

    for (const auto& r : roots) {
        std::wstring dll = FindNewestEmbeddedWebViewDllInRoot(r);
        if (! dll.empty()) return dll;
    }
    return {};
}

void CleanupStaleTempUserDataDirs(const std::wstring& tempRoot, DWORD currentPid) {
    std::error_code ec;
    const std::wstring prefix = L"LcdFusion_WpeWebView2_";
    for (const auto& entry : std::filesystem::directory_iterator(tempRoot, ec)) {
        if (! entry.is_directory(ec)) continue;
        std::wstring name = entry.path().filename().wstring();
        if (name.rfind(prefix, 0) != 0) continue;
        std::wstring pidStr = name.substr(prefix.size());
        DWORD        pid    = 0;
        try {
            pid = static_cast<DWORD>(std::stoul(pidStr));
        } catch (...) {
            continue;
        }
        if (pid == 0 || pid == currentPid) continue;
        bool   alive = false;
        HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (hProc) {
            DWORD code = 0;
            if (GetExitCodeProcess(hProc, &code) && code == STILL_ACTIVE) alive = true;
            CloseHandle(hProc);
        }
        if (! alive) {
            std::error_code remEc;
            std::filesystem::remove_all(entry.path(), remEc);
        }
    }
}

using PfnCreateEnvLoader = HRESULT(STDMETHODCALLTYPE*)(
    PCWSTR browserExecutableFolder, PCWSTR userDataFolder,
    ICoreWebView2EnvironmentOptions* environmentOptions,
    ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler* environmentCreatedHandler);

using PfnCreateEnvInternal = HRESULT(STDMETHODCALLTYPE*)(
    bool checkRunningInstance, int runtimeType, PCWSTR userDataFolder, IUnknown* envOptions,
    ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler* handler);

} // namespace

struct WebWallpaperWin32::Impl {
    FrameConsumer frameConsumer;

    std::thread             workerThread;
    std::mutex              initMutex;
    std::condition_variable initCv;
    bool                    initDone { false };
    bool                    initOk { false };
    std::string             initError;
    std::string             initCode;

    std::atomic<bool>     open { false };
    std::atomic<bool>     closing { false };
    std::atomic<bool>     paused { false };
    std::atomic<int>      fitMode { 0 };
    std::atomic<uint32_t> width { 480 };
    std::atomic<uint32_t> height { 480 };
    std::atomic<DWORD>    workerThreadId { 0 };

    void ComputeViewportSize(uint32_t& vpW, uint32_t& vpH) const {
        uint32_t outW = width.load();
        uint32_t outH = height.load();
        int      fm   = fitMode.load();
        if (fm == 0) {
            vpW = outW;
            vpH = outH;
        } else {
            // 16:9 logical viewport for Fit (letterbox) & Stretch (full-canvas deform)
            uint32_t baseW = std::clamp<uint32_t>(std::max<uint32_t>(outW, static_cast<uint32_t>(std::lround(outH * 16.0 / 9.0))), 640, 1280);
            vpW            = baseW;
            vpH            = std::max<uint32_t>(360, static_cast<uint32_t>(std::lround(baseW * 9.0 / 16.0)));
        }
    }

    // Shared latest decoded BGRA32 frame (written by STA thread, read by RenderFrame thread)
    std::mutex           frameMutex;
    std::vector<uint8_t> latestBgra;
    uint32_t             latestW { 0 };
    uint32_t             latestH { 0 };
    uint64_t             latestGen { 0 };
    uint64_t             deliveredGen { 0 };

    // Pending async payloads posted to STA thread
    std::mutex           cmdMutex;
    std::string          pendingPropsJson;
    std::string          pendingSensorsJson;
    std::string          lastSensorsJson;
    std::vector<float>   pendingAudio128;
    std::atomic<bool>    audioPending { false };
    std::chrono::steady_clock::time_point lastAudioPush {};

    // STA-thread-only state
    HWND                                      hwnd { nullptr };
    HMODULE                                   webViewModule { nullptr };
    ICoreWebView2Environment*                 env { nullptr };
    ICoreWebView2Controller*                  controller { nullptr };
    ICoreWebView2*                            webView { nullptr };
    ICoreWebView2DevToolsProtocolEventReceiver* screencastReceiver { nullptr };
    EventRegistrationToken                    screencastToken {};
    bool                                      screencastActive { false };
    bool                                      captureInFlight { false };
    std::wstring                              userDataDir;
    std::string                               initialPropsJson;
    std::wstring                              wallpaperDirWide;
    std::wstring                              entryUrlWide;
    std::chrono::steady_clock::time_point     lastScreencastFrameTime {};

    static LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
        if (msg == WM_CREATE) {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
            SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
            return 0;
        }
        auto* self = reinterpret_cast<Impl*>(GetWindowLongPtrW(h, GWLP_USERDATA));
        if (! self) return DefWindowProcW(h, msg, wp, lp);

        switch (msg) {
        case WM_WPE_RESIZE:
            self->OnStaResize(static_cast<uint32_t>(wp), static_cast<uint32_t>(lp));
            return 0;
        case WM_WPE_PROPS:
            self->OnStaApplyProps();
            return 0;
        case WM_WPE_PAUSED:
            self->OnStaSetPaused(wp != 0);
            return 0;
        case WM_WPE_AUDIO:
            self->OnStaPushAudio();
            return 0;
        case WM_WPE_SENSORS:
            self->OnStaPushSensors();
            return 0;
        case WM_WPE_CLOSE:
            self->OnStaCleanupAndQuit();
            return 0;
        case WM_TIMER:
            if (wp == 1) {
                self->OnStaFallbackCaptureTick();
                return 0;
            }
            break;
        default:
            break;
        }
        return DefWindowProcW(h, msg, wp, lp);
    }

    void SignalInitResult(bool ok, const std::string& err, const std::string& code) {
        {
            std::lock_guard<std::mutex> lk(initMutex);
            if (initDone) return;
            initOk    = ok;
            initError = err;
            initCode  = code;
            initDone  = true;
        }
        initCv.notify_all();
    }

    void StoreDecodedRgba(const uint8_t* rgba, uint32_t srcW, uint32_t srcH) {
        if (! rgba || srcW == 0 || srcH == 0) return;
        const uint32_t dstW = width.load();
        const uint32_t dstH = height.load();
        if (dstW == 0 || dstH == 0) return;

        // Drop stale in-flight screencast frames left over from before a viewport/fitMode resize
        uint32_t expW = 0, expH = 0;
        ComputeViewportSize(expW, expH);
        if (expW > 0 && expH > 0) {
            double srcAspect = static_cast<double>(srcW) / static_cast<double>(srcH);
            double expAspect = static_cast<double>(expW) / static_cast<double>(expH);
            if (std::abs(srcAspect - expAspect) / expAspect > 0.12) {
                return;
            }
        }

        std::vector<uint8_t> bgra(static_cast<std::size_t>(dstW) * dstH * 4u);
        RgbaToBgraWithFit(rgba, srcW, srcH, bgra.data(), dstW, dstH, fitMode.load());

        std::lock_guard<std::mutex> lk(frameMutex);
        latestBgra.swap(bgra);
        latestW = dstW;
        latestH = dstH;
        ++latestGen;
    }

    void StartScreencast() {
        if (! webView || paused.load() || closing.load()) return;
        uint32_t w = 0, h = 0;
        ComputeViewportSize(w, h);
        nlohmann::json params = {
            { "format", "jpeg" },
            { "quality", 90 },
            { "maxWidth", w },
            { "maxHeight", h },
            { "everyNthFrame", 1 },
        };
        std::wstring wParams = Utf8ToWide(params.dump());
        auto*        cb      = new ComHandler<ICoreWebView2CallDevToolsProtocolMethodCompletedHandler,
                                       &kIidCdpMethod, HRESULT, LPCWSTR>(
            [this](HRESULT hr, LPCWSTR /*res*/) -> HRESULT {
                if (FAILED(hr)) {
                    LOG_ERROR("WebView2 Page.startScreencast failed: 0x%08lX", static_cast<unsigned long>(hr));
                }
                return S_OK;
            });
        webView->CallDevToolsProtocolMethod(L"Page.startScreencast", wParams.c_str(), cb);
        cb->Release();
        screencastActive = true;
    }

    void StopScreencast() {
        if (! webView || ! screencastActive) return;
        auto* cb = new ComHandler<ICoreWebView2CallDevToolsProtocolMethodCompletedHandler,
                                  &kIidCdpMethod, HRESULT, LPCWSTR>(
            [](HRESULT, LPCWSTR) -> HRESULT { return S_OK; });
        webView->CallDevToolsProtocolMethod(L"Page.stopScreencast", L"{}", cb);
        cb->Release();
        screencastActive = false;
    }

    void TriggerCapturePreviewFallback() {
        if (! webView || captureInFlight || paused.load() || closing.load()) return;
        IStream* stream = SHCreateMemStream(nullptr, 0);
        if (! stream) return;
        captureInFlight = true;

        auto* cb = new ComHandler<ICoreWebView2CapturePreviewCompletedHandler,
                                  &kIidCapturePreview, HRESULT>(
            [this, stream](HRESULT hr) -> HRESULT {
                captureInFlight = false;
                if (SUCCEEDED(hr) && ! closing.load()) {
                    STATSTG stat {};
                    if (SUCCEEDED(stream->Stat(&stat, STATFLAG_NONAME)) && stat.cbSize.QuadPart > 0 &&
                        stat.cbSize.QuadPart < 32 * 1024 * 1024) {
                        LARGE_INTEGER zero {};
                        stream->Seek(zero, STREAM_SEEK_SET, nullptr);
                        std::vector<uint8_t> bytes(static_cast<std::size_t>(stat.cbSize.QuadPart));
                        ULONG                read = 0;
                        if (SUCCEEDED(stream->Read(bytes.data(), static_cast<ULONG>(bytes.size()), &read)) &&
                            read > 0) {
                            int      w = 0, h = 0, ch = 0;
                            stbi_uc* rgba = stbi_load_from_memory(bytes.data(), static_cast<int>(read), &w, &h, &ch, 4);
                            if (rgba) {
                                StoreDecodedRgba(rgba, static_cast<uint32_t>(w), static_cast<uint32_t>(h));
                                stbi_image_free(rgba);
                            }
                        }
                    }
                }
                stream->Release();
                return S_OK;
            });

        HRESULT hr = webView->CapturePreview(COREWEBVIEW2_CAPTURE_PREVIEW_IMAGE_FORMAT_JPEG, stream, cb);
        cb->Release();
        if (FAILED(hr)) {
            captureInFlight = false;
            stream->Release();
        }
    }

    void OnStaFallbackCaptureTick() {
        if (! webView || paused.load() || closing.load()) return;
        auto now = std::chrono::steady_clock::now();
        // Only run CapturePreview if screencast hasn't delivered a frame in > 1.2s (e.g., static HTML page)
        if (lastScreencastFrameTime.time_since_epoch().count() == 0 ||
            std::chrono::duration_cast<std::chrono::milliseconds>(now - lastScreencastFrameTime).count() > 1200) {
            TriggerCapturePreviewFallback();
        }
    }

    void OnScreencastFrame(LPCWSTR jsonWide) {
        if (! jsonWide || closing.load()) return;
        try {
            auto j = nlohmann::json::parse(WideToUtf8(jsonWide));
            if (j.contains("sessionId")) {
                int sessionId = j["sessionId"].get<int>();
                std::wstring ackParams = L"{\"sessionId\":" + std::to_wstring(sessionId) + L"}";
                auto*        ackCb     = new ComHandler<ICoreWebView2CallDevToolsProtocolMethodCompletedHandler,
                                                &kIidCdpMethod, HRESULT, LPCWSTR>(
                    [](HRESULT, LPCWSTR) -> HRESULT { return S_OK; });
                if (webView) {
                    webView->CallDevToolsProtocolMethod(L"Page.screencastFrameAck", ackParams.c_str(), ackCb);
                }
                ackCb->Release();
            }
            if (j.contains("data") && j["data"].is_string()) {
                auto encoded = Base64Decode(j["data"].get_ref<const std::string&>());
                if (! encoded.empty()) {
                    int      w = 0, h = 0, ch = 0;
                    stbi_uc* rgba = stbi_load_from_memory(
                        encoded.data(), static_cast<int>(encoded.size()), &w, &h, &ch, 4);
                    if (rgba) {
                        lastScreencastFrameTime = std::chrono::steady_clock::now();
                        StoreDecodedRgba(rgba, static_cast<uint32_t>(w), static_cast<uint32_t>(h));
                        stbi_image_free(rgba);
                    }
                }
            }
        } catch (const std::exception& ex) {
            LOG_ERROR("WebView2 screencastFrame parse error: %s", ex.what());
        }
    }

    void NotifyPageResize() {
        if (! webView) return;
        // Many Web wallpapers (e.g. Spine WebGL) size their <canvas> via HTML width/height attributes once
        // on startup and read canvas.clientWidth inside a global resize() function without CSS 100vw/100vh
        // or a window 'resize' listener. Sync unsized canvases to the new viewport before invoking resize().
        static constexpr const wchar_t* kResizeJs = LR"JS(
(function() {
    function run() {
        var iw = window.innerWidth, ih = window.innerHeight;
        if (iw > 0 && ih > 0) {
            var list = document.querySelectorAll('canvas');
            for (var i = 0; i < list.length; i++) {
                var c = list[i];
                if (!c.style.width && !c.style.height) {
                    if (c.width !== iw) c.width = iw;
                    if (c.height !== ih) c.height = ih;
                }
            }
        }
        try { window.dispatchEvent(new Event('resize')); } catch (e) {}
        if (typeof window.resize === 'function') {
            try { window.resize(); } catch (e) {}
        }
    }
    requestAnimationFrame(run);
    setTimeout(run, 80);
})();
)JS";
        webView->ExecuteScript(kResizeJs, nullptr);
    }

    void OnStaResize(uint32_t w, uint32_t h) {
        width.store(w);
        height.store(h);
        uint32_t vpW = 0, vpH = 0;
        ComputeViewportSize(vpW, vpH);
        if (hwnd) {
            SetWindowPos(hwnd, nullptr, -32000, -32000, static_cast<int>(vpW), static_cast<int>(vpH),
                         SWP_NOZORDER | SWP_NOACTIVATE);
        }
        if (controller) {
            RECT rc { 0, 0, static_cast<LONG>(vpW), static_cast<LONG>(vpH) };
            controller->put_Bounds(rc);
        }
        if (webView) {
            NotifyPageResize();
            if (! paused.load()) {
                StopScreencast();
                StartScreencast();
            }
        }
    }

    void OnStaApplyProps() {
        if (! webView) return;
        std::string props;
        {
            std::lock_guard<std::mutex> lk(cmdMutex);
            props.swap(pendingPropsJson);
        }
        if (props.empty()) return;
        std::wstring script = L"if(window.__wpeApplyProps){window.__wpeApplyProps(" + Utf8ToWide(props) + L");}";
        webView->ExecuteScript(script.c_str(), nullptr);
    }

    void OnStaPushSensors() {
        if (! webView) return;
        std::string sensors;
        {
            std::lock_guard<std::mutex> lk(cmdMutex);
            sensors.swap(pendingSensorsJson);
        }
        if (sensors.empty()) return;
        std::wstring script = L"if(window.__wpePushSensors){window.__wpePushSensors(" + Utf8ToWide(sensors) + L");}";
        webView->ExecuteScript(script.c_str(), nullptr);
    }

    void OnStaSetPaused(bool isPaused) {
        if (! webView) return;
        std::wstring script = isPaused
                                  ? L"if(window.__wpeSetPaused){window.__wpeSetPaused(true);}"
                                  : L"if(window.__wpeSetPaused){window.__wpeSetPaused(false);}";
        webView->ExecuteScript(script.c_str(), nullptr);
        if (isPaused) {
            StopScreencast();
        } else {
            StartScreencast();
        }
    }

    void OnStaPushAudio() {
        audioPending.store(false);
        if (! webView || paused.load()) return;
        std::vector<float> arr;
        {
            std::lock_guard<std::mutex> lk(cmdMutex);
            arr.swap(pendingAudio128);
        }
        if (arr.size() != 128) return;
        std::ostringstream oss;
        oss << "if(window.__wpeHasAudioListener && window.__wpeHasAudioListener()){window.__wpePushAudio([";
        for (std::size_t i = 0; i < 128; ++i) {
            if (i > 0) oss << ',';
            int scaled = static_cast<int>(std::lround(std::clamp(arr[i], 0.0f, 4.0f) * 1000.0f));
            if (scaled == 0)
                oss << '0';
            else
                oss << (scaled / 1000.0f);
        }
        oss << "]);}";
        std::wstring wScript = Utf8ToWide(oss.str());
        webView->ExecuteScript(wScript.c_str(), nullptr);
    }

    void OnStaCleanupAndQuit() {
        if (hwnd) KillTimer(hwnd, 1);
        StopScreencast();
        if (screencastReceiver) {
            screencastReceiver->remove_DevToolsProtocolEventReceived(screencastToken);
            screencastReceiver->Release();
            screencastReceiver = nullptr;
        }
        if (controller) {
            controller->Close();
            controller->Release();
            controller = nullptr;
        }
        if (webView) {
            webView->Release();
            webView = nullptr;
        }
        if (env) {
            env->Release();
            env = nullptr;
        }
        if (hwnd) {
            DestroyWindow(hwnd);
            hwnd = nullptr;
        }
        PostQuitMessage(0);
    }

    void OnControllerCreated(HRESULT hr, ICoreWebView2Controller* ctrl) {
        if (FAILED(hr) || ! ctrl || closing.load()) {
            std::ostringstream oss;
            oss << "WebView2 controller creation failed (HRESULT 0x" << std::hex << static_cast<unsigned long>(hr) << ")";
            SignalInitResult(false, oss.str(), "WEBVIEW2_INIT_FAILED");
            PostQuitMessage(0);
            return;
        }

        controller = ctrl;
        controller->AddRef();

        if (FAILED(controller->get_CoreWebView2(&webView)) || ! webView) {
            SignalInitResult(false, "Failed to acquire ICoreWebView2 from controller", "WEBVIEW2_INIT_FAILED");
            OnStaCleanupAndQuit();
            return;
        }

        // Force 1:1 raw-pixel rendering so host monitor DPI does not skew output dimensions
        ICoreWebView2Controller3* ctrl3 = nullptr;
        if (SUCCEEDED(controller->QueryInterface(kIidController3, reinterpret_cast<void**>(&ctrl3))) && ctrl3) {
            ctrl3->put_BoundsMode(COREWEBVIEW2_BOUNDS_MODE_USE_RAW_PIXELS);
            ctrl3->put_ShouldDetectMonitorScaleChanges(FALSE);
            ctrl3->put_RasterizationScale(1.0);
            ctrl3->Release();
        }

        uint32_t vpW = 0, vpH = 0;
        ComputeViewportSize(vpW, vpH);
        RECT bounds { 0, 0, static_cast<LONG>(vpW), static_cast<LONG>(vpH) };
        controller->put_Bounds(bounds);
        controller->put_IsVisible(TRUE);

        // Configure quiet settings
        ICoreWebView2Settings* settings = nullptr;
        if (SUCCEEDED(webView->get_Settings(&settings)) && settings) {
            settings->put_AreDefaultContextMenusEnabled(FALSE);
            settings->put_AreDevToolsEnabled(FALSE);
            settings->put_IsStatusBarEnabled(FALSE);
            settings->put_IsZoomControlEnabled(FALSE);
            settings->put_AreDefaultScriptDialogsEnabled(FALSE);
            settings->Release();
        }

        // Mute audio cleanly on WebView2_8
        ICoreWebView2_8* wv8 = nullptr;
        if (SUCCEEDED(webView->QueryInterface(kIidWebView2_8, reinterpret_cast<void**>(&wv8))) && wv8) {
            wv8->put_IsMuted(TRUE);
            wv8->Release();
        }

        // Map local wallpaper directory to https://wallpaper.local/ (zero CORS issues for WebGL/Spine/fetch)
        ICoreWebView2_3* wv3 = nullptr;
        if (SUCCEEDED(webView->QueryInterface(kIidWebView2_3, reinterpret_cast<void**>(&wv3))) && wv3) {
            HRESULT mapHr = wv3->SetVirtualHostNameToFolderMapping(
                L"wallpaper.local", wallpaperDirWide.c_str(), COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW);
            if (FAILED(mapHr)) {
                LOG_ERROR("WebView2 SetVirtualHostNameToFolderMapping failed: 0x%08lX", static_cast<unsigned long>(mapHr));
            }
            wv3->Release();
        }

        // Inject Wallpaper Engine Web API shim + initial properties before document scripts execute
        std::wstring initScript = kWebWallpaperBridgeScript;
        if (! initialPropsJson.empty() && initialPropsJson != "{}") {
            initScript += L"\nwindow.__wpeApplyProps(" + Utf8ToWide(initialPropsJson) + L");\n";
        }
        webView->AddScriptToExecuteOnDocumentCreated(initScript.c_str(), nullptr);

        // Hook CDP Page.screencastFrame for continuous GPU-composited offscreen frames
        if (SUCCEEDED(webView->GetDevToolsProtocolEventReceiver(L"Page.screencastFrame", &screencastReceiver)) &&
            screencastReceiver) {
            auto* evHandler = new ComHandler<ICoreWebView2DevToolsProtocolEventReceivedEventHandler,
                                             &kIidCdpEvent, ICoreWebView2*,
                                             ICoreWebView2DevToolsProtocolEventReceivedEventArgs*>(
                [this](ICoreWebView2*, ICoreWebView2DevToolsProtocolEventReceivedEventArgs* args) -> HRESULT {
                    if (! args) return S_OK;
                    LPWSTR json = nullptr;
                    if (SUCCEEDED(args->get_ParameterObjectAsJson(&json)) && json) {
                        OnScreencastFrame(json);
                        CoTaskMemFree(json);
                    }
                    return S_OK;
                });
            screencastReceiver->add_DevToolsProtocolEventReceived(evHandler, &screencastToken);
            evHandler->Release();
        }

        // Start screencast on NavigationCompleted too (ensures first frame is always captured)
        auto* navHandler = new ComHandler<ICoreWebView2NavigationCompletedEventHandler,
                                          &kIidNavCompleted, ICoreWebView2*,
                                          ICoreWebView2NavigationCompletedEventArgs*>(
            [this](ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs*) -> HRESULT {
                if (! closing.load()) {
                    std::string cachedSensors;
                    {
                        std::lock_guard<std::mutex> lk(cmdMutex);
                        cachedSensors = lastSensorsJson;
                    }
                    if (! cachedSensors.empty() && webView) {
                        std::wstring s = L"if(window.__wpePushSensors){window.__wpePushSensors(" + Utf8ToWide(cachedSensors) + L");}";
                        webView->ExecuteScript(s.c_str(), nullptr);
                    }
                }
                if (! paused.load() && ! closing.load()) {
                    if (! screencastActive) StartScreencast();
                    TriggerCapturePreviewFallback();
                }
                return S_OK;
            });
        EventRegistrationToken navTok {};
        webView->add_NavigationCompleted(navHandler, &navTok);
        navHandler->Release();

        // Fallback timer for completely static web pages (every 600ms if no screencast frame)
        SetTimer(hwnd, 1, 600, nullptr);

        StartScreencast();
        HRESULT navHr = webView->Navigate(entryUrlWide.c_str());
        if (FAILED(navHr)) {
            SignalInitResult(false, "WebView2 Navigate failed", "WEBVIEW2_INIT_FAILED");
            OnStaCleanupAndQuit();
            return;
        }

        open.store(true);
        SignalInitResult(true, "", "");
    }

    void OnEnvCreated(HRESULT hr, ICoreWebView2Environment* createdEnv) {
        if (FAILED(hr) || ! createdEnv || closing.load()) {
            std::ostringstream oss;
            oss << "WebView2 environment creation failed (HRESULT 0x" << std::hex << static_cast<unsigned long>(hr) << ")";
            SignalInitResult(false, oss.str(), "WEBVIEW2_INIT_FAILED");
            PostQuitMessage(0);
            return;
        }
        env = createdEnv;
        env->AddRef();

        auto* ctrlHandler = new ComHandler<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler,
                                           &kIidCtrlCompleted, HRESULT, ICoreWebView2Controller*>(
            [this](HRESULT cHr, ICoreWebView2Controller* ctrl) -> HRESULT {
                OnControllerCreated(cHr, ctrl);
                return S_OK;
            });
        HRESULT r = env->CreateCoreWebView2Controller(hwnd, ctrlHandler);
        ctrlHandler->Release();
        if (FAILED(r)) {
            SignalInitResult(false, "CreateCoreWebView2Controller call failed", "WEBVIEW2_INIT_FAILED");
            OnStaCleanupAndQuit();
        }
    }

    void WorkerMain() {
        workerThreadId.store(GetCurrentThreadId());

        HRESULT comHr = OleInitialize(nullptr);
        if (FAILED(comHr)) {
            SignalInitResult(false, "OleInitialize failed on WebView2 STA thread", "WEBVIEW2_INIT_FAILED");
            return;
        }

        // Register & create cloaked offscreen tool window
        static const wchar_t* kClsName = L"LcdFusionWpeWebViewHost";
        WNDCLASSEXW           wc {};
        wc.cbSize        = sizeof(wc);
        wc.lpfnWndProc   = WndProc;
        wc.hInstance     = GetModuleHandleW(nullptr);
        wc.lpszClassName = kClsName;
        RegisterClassExW(&wc);

        uint32_t initVpW = 0, initVpH = 0;
        ComputeViewportSize(initVpW, initVpH);
        hwnd = CreateWindowExW(
            WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
            kClsName,
            L"LcdFusion WebView2 Host",
            WS_POPUP | WS_CLIPSIBLINGS | WS_CLIPCHILDREN,
            -32000,
            -32000,
            static_cast<int>(initVpW),
            static_cast<int>(initVpH),
            nullptr,
            nullptr,
            GetModuleHandleW(nullptr),
            this);
        if (! hwnd) {
            SignalInitResult(false, "Failed to create offscreen WebView2 host window", "WEBVIEW2_INIT_FAILED");
            OleUninitialize();
            return;
        }

        BOOL cloak = TRUE;
        DwmSetWindowAttribute(hwnd, DWMWA_CLOAK, &cloak, sizeof(cloak));
        ShowWindow(hwnd, SW_SHOWNOACTIVATE);

        // Unique per-PID user data directory in %TEMP% so multi-screen Web wallpapers never collide;
        // also sweep any stale folders left behind by previously exited/killed renderer instances.
        wchar_t tempBuf[MAX_PATH] = {};
        GetTempPathW(MAX_PATH, tempBuf);
        const DWORD selfPid = GetCurrentProcessId();
        CleanupStaleTempUserDataDirs(tempBuf, selfPid);
        userDataDir = std::wstring(tempBuf) + L"LcdFusion_WpeWebView2_" + std::to_wstring(selfPid);
        std::error_code ec;
        std::filesystem::create_directories(userDataDir, ec);

        auto* envOpts = new SimpleEnvOptions(
            L"--force-device-scale-factor=1 "
            L"--autoplay-policy=no-user-gesture-required "
            L"--disable-background-timer-throttling "
            L"--disable-renderer-backgrounding "
            L"--disable-backgrounding-occluded-windows "
            L"--mute-audio");

        auto* envHandler = new ComHandler<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler,
                                          &kIidEnvCompleted, HRESULT, ICoreWebView2Environment*>(
            [this](HRESULT eHr, ICoreWebView2Environment* createdEnv) -> HRESULT {
                OnEnvCreated(eHr, createdEnv);
                return S_OK;
            });

        HRESULT createHr = E_FAIL;

        // 1. Try standard WebView2Loader.dll if present next to exe or on PATH
        webViewModule = LoadLibraryW(L"WebView2Loader.dll");
        if (webViewModule) {
            auto pfn = reinterpret_cast<PfnCreateEnvLoader>(
                GetProcAddress(webViewModule, "CreateCoreWebView2EnvironmentWithOptions"));
            if (pfn) {
                createHr = pfn(nullptr, userDataDir.c_str(), envOpts, envHandler);
            }
        }

        // 2. Zero-DLL fallback: load EmbeddedBrowserWebView.dll directly from installed Edge WebView2 Runtime
        if (FAILED(createHr)) {
            if (webViewModule) {
                FreeLibrary(webViewModule);
                webViewModule = nullptr;
            }
            std::wstring dllPath = LocateEmbeddedWebView2Dll();
            if (dllPath.empty()) {
                envHandler->Release();
                envOpts->Release();
                DestroyWindow(hwnd);
                hwnd = nullptr;
                SignalInitResult(false,
                                 "Microsoft Edge WebView2 Runtime not found on this machine",
                                 "WEBVIEW2_NOT_FOUND");
                OleUninitialize();
                return;
            }

            webViewModule = LoadLibraryExW(dllPath.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
            if (! webViewModule) {
                envHandler->Release();
                envOpts->Release();
                DestroyWindow(hwnd);
                hwnd = nullptr;
                SignalInitResult(false,
                                 "Failed to load EmbeddedBrowserWebView.dll",
                                 "WEBVIEW2_INIT_FAILED");
                OleUninitialize();
                return;
            }

            auto pfnInternal = reinterpret_cast<PfnCreateEnvInternal>(
                GetProcAddress(webViewModule, "CreateWebViewEnvironmentWithOptionsInternal"));
            if (! pfnInternal) {
                envHandler->Release();
                envOpts->Release();
                DestroyWindow(hwnd);
                hwnd = nullptr;
                SignalInitResult(false,
                                 "CreateWebViewEnvironmentWithOptionsInternal export not found",
                                 "WEBVIEW2_INIT_FAILED");
                OleUninitialize();
                return;
            }

            // runtimeType = 0 (Installed Evergreen WebView2 Runtime)
            createHr = pfnInternal(true, 0, userDataDir.c_str(), envOpts, envHandler);
        }

        envHandler->Release();
        envOpts->Release();

        if (FAILED(createHr)) {
            std::ostringstream oss;
            oss << "WebView2 bootstrap failed (HRESULT 0x" << std::hex << static_cast<unsigned long>(createHr) << ")";
            DestroyWindow(hwnd);
            hwnd = nullptr;
            SignalInitResult(false, oss.str(), "WEBVIEW2_INIT_FAILED");
            OleUninitialize();
            return;
        }

        // Pump STA messages until WM_WPE_CLOSE posts WM_QUIT
        MSG msg {};
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        if (hwnd) {
            DestroyWindow(hwnd);
            hwnd = nullptr;
        }
        if (webViewModule) {
            // Keep EmbeddedBrowserWebView.dll loaded until process exit to avoid Chromium teardown races
            webViewModule = nullptr;
        }
        OleUninitialize();

        // Best-effort cleanup of per-process temporary WebView2 user-data directory
        if (! userDataDir.empty()) {
            for (int attempt = 0; attempt < 3; ++attempt) {
                std::error_code remEc;
                std::filesystem::remove_all(userDataDir, remEc);
                if (! std::filesystem::exists(userDataDir, remEc)) break;
                Sleep(120);
            }
        }
    }
};

WebWallpaperWin32::WebWallpaperWin32(): m_impl(std::make_unique<Impl>()) {}
WebWallpaperWin32::~WebWallpaperWin32() { Close(); }

void WebWallpaperWin32::SetFrameConsumer(FrameConsumer consumer) {
    if (m_impl) m_impl->frameConsumer = std::move(consumer);
}

void WebWallpaperWin32::SetFitMode(int fitMode) {
    if (! m_impl) return;
    int clamped = (fitMode == 1 || fitMode == 2) ? fitMode : 0;
    if (m_impl->fitMode.exchange(clamped) != clamped && m_impl->open.load() && m_impl->hwnd) {
        PostMessageW(m_impl->hwnd,
                     WM_WPE_RESIZE,
                     static_cast<WPARAM>(m_impl->width.load()),
                     static_cast<LPARAM>(m_impl->height.load()));
    }
}

bool WebWallpaperWin32::IsOpen() const { return m_impl && m_impl->open.load(); }
uint32_t WebWallpaperWin32::Width() const { return m_impl ? m_impl->width.load() : 0; }
uint32_t WebWallpaperWin32::Height() const { return m_impl ? m_impl->height.load() : 0; }

bool WebWallpaperWin32::Open(const std::string& wallpaperDirUtf8, const std::string& entryFileUtf8,
                             uint32_t width, uint32_t height, const std::string& initialPropsJson,
                             std::string& outError, std::string& outCode) {
    Close();

    m_impl->closing.store(false);
    m_impl->open.store(false);
    m_impl->paused.store(false);
    m_impl->width.store(std::clamp<uint32_t>(width, 64, 2048));
    m_impl->height.store(std::clamp<uint32_t>(height, 64, 2048));
    m_impl->initialPropsJson = initialPropsJson;
    m_impl->wallpaperDirWide = Utf8ToWide(wallpaperDirUtf8);

    std::string rel = entryFileUtf8.empty() ? "index.html" : entryFileUtf8;
    m_impl->entryUrlWide = Utf8ToWide("https://wallpaper.local/" + UrlEncodeRelPath(rel));

    {
        std::lock_guard<std::mutex> lk(m_impl->initMutex);
        m_impl->initDone = false;
        m_impl->initOk   = false;
        m_impl->initError.clear();
        m_impl->initCode.clear();
    }
    {
        std::lock_guard<std::mutex> lk(m_impl->frameMutex);
        m_impl->latestBgra.clear();
        m_impl->latestW      = 0;
        m_impl->latestH      = 0;
        m_impl->latestGen    = 0;
        m_impl->deliveredGen = 0;
    }

    m_impl->workerThread = std::thread([this]() { m_impl->WorkerMain(); });

    std::unique_lock<std::mutex> lk(m_impl->initMutex);
    bool signaled = m_impl->initCv.wait_for(
        lk, std::chrono::seconds(15), [this]() { return m_impl->initDone; });
    if (! signaled) {
        lk.unlock();
        outError = "Timed out initializing WebView2 runtime";
        outCode  = "WEBVIEW2_INIT_FAILED";
        Close();
        return false;
    }

    if (! m_impl->initOk) {
        outError = m_impl->initError;
        outCode  = m_impl->initCode.empty() ? "WEBVIEW2_INIT_FAILED" : m_impl->initCode;
        lk.unlock();
        Close();
        return false;
    }

    outCode.clear();
    return true;
}

bool WebWallpaperWin32::Resize(uint32_t width, uint32_t height) {
    if (! m_impl || ! m_impl->open.load()) return false;
    uint32_t w = std::clamp<uint32_t>(width, 64, 2048);
    uint32_t h = std::clamp<uint32_t>(height, 64, 2048);
    if (m_impl->hwnd) {
        PostMessageW(m_impl->hwnd, WM_WPE_RESIZE, static_cast<WPARAM>(w), static_cast<LPARAM>(h));
    }
    return true;
}

void WebWallpaperWin32::SetProperties(const std::string& changedPropsJson) {
    if (! m_impl || ! m_impl->open.load() || changedPropsJson.empty()) return;
    {
        std::lock_guard<std::mutex> lk(m_impl->cmdMutex);
        m_impl->pendingPropsJson = changedPropsJson;
    }
    if (m_impl->hwnd) {
        PostMessageW(m_impl->hwnd, WM_WPE_PROPS, 0, 0);
    }
}

void WebWallpaperWin32::SetSensors(const std::string& sensorsJson) {
    if (! m_impl || sensorsJson.empty()) return;
    {
        std::lock_guard<std::mutex> lk(m_impl->cmdMutex);
        m_impl->lastSensorsJson    = sensorsJson;
        m_impl->pendingSensorsJson = sensorsJson;
    }
    if (m_impl->open.load() && m_impl->hwnd) {
        PostMessageW(m_impl->hwnd, WM_WPE_SENSORS, 0, 0);
    }
}

void WebWallpaperWin32::SetPaused(bool paused) {
    if (! m_impl) return;
    m_impl->paused.store(paused);
    if (m_impl->open.load() && m_impl->hwnd) {
        PostMessageW(m_impl->hwnd, WM_WPE_PAUSED, paused ? 1 : 0, 0);
    }
}

void WebWallpaperWin32::FeedAudioSpectrum(const AudioSpectrumData& audio) {
    if (! m_impl || ! m_impl->open.load() || m_impl->paused.load()) return;
    auto now = std::chrono::steady_clock::now();
    if (std::chrono::duration_cast<std::chrono::milliseconds>(now - m_impl->lastAudioPush).count() < 40) {
        return;
    }
    if (m_impl->audioPending.exchange(true)) return;
    m_impl->lastAudioPush = now;

    {
        std::lock_guard<std::mutex> lk(m_impl->cmdMutex);
        m_impl->pendingAudio128.resize(128);
        for (std::size_t i = 0; i < 64; ++i) {
            m_impl->pendingAudio128[i]      = audio.left64[i];
            m_impl->pendingAudio128[64 + i] = audio.right64[i];
        }
    }
    if (m_impl->hwnd) {
        PostMessageW(m_impl->hwnd, WM_WPE_AUDIO, 0, 0);
    } else {
        m_impl->audioPending.store(false);
    }
}

bool WebWallpaperWin32::RenderFrame(double /*deltaSec*/) {
    if (! m_impl || ! m_impl->open.load()) return false;

    std::vector<uint8_t> frameCopy;
    uint32_t             w = 0, h = 0;
    {
        std::lock_guard<std::mutex> lk(m_impl->frameMutex);
        if (m_impl->latestBgra.empty() || m_impl->latestW == 0 || m_impl->latestH == 0) {
            return true; // Still warming up initial WebView2 load
        }
        if (m_impl->latestGen == m_impl->deliveredGen) {
            return true; // No new composited frame yet
        }
        frameCopy            = m_impl->latestBgra;
        w                    = m_impl->latestW;
        h                    = m_impl->latestH;
        m_impl->deliveredGen = m_impl->latestGen;
    }

    if (m_impl->frameConsumer && ! frameCopy.empty()) {
        m_impl->frameConsumer(frameCopy.data(), w, h, w * 4u);
    }
    return true;
}

void WebWallpaperWin32::Close() {
    if (! m_impl) return;
    m_impl->closing.store(true);
    m_impl->open.store(false);

    if (m_impl->workerThread.joinable()) {
        if (m_impl->hwnd) {
            PostMessageW(m_impl->hwnd, WM_WPE_CLOSE, 0, 0);
        } else {
            DWORD tid = m_impl->workerThreadId.load();
            if (tid != 0) PostThreadMessageW(tid, WM_QUIT, 0, 0);
        }
        m_impl->workerThread.join();
    }
    m_impl->workerThreadId.store(0);
}

} // namespace lcdfusion_wpe

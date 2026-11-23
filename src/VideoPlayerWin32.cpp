#include "VideoPlayerWin32.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <objbase.h>
#include <d3d11.h>
#include <dxgi.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <propvarutil.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <sstream>

namespace lcdfusion_wpe
{

namespace
{

std::wstring Utf8ToWideVideo(const std::string& s) {
    if (s.empty()) return {};
    int needed = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (needed <= 0) return {};
    std::wstring out(static_cast<std::size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), needed);
    return out;
}

std::string HexHr(HRESULT hr) {
    std::ostringstream oss;
    oss << "0x" << std::hex << std::uppercase << static_cast<uint32_t>(hr);
    return oss.str();
}

bool TryCreateDxgiDeviceManagerForLuid(int32_t luidHigh, uint32_t luidLow,
                                       ID3D11Device**         outDevice,
                                       IMFDXGIDeviceManager** outManager) {
    *outDevice  = nullptr;
    *outManager = nullptr;

    HMODULE hDxgi   = LoadLibraryW(L"dxgi.dll");
    HMODULE hD3d11  = LoadLibraryW(L"d3d11.dll");
    HMODULE hMfplat = GetModuleHandleW(L"mfplat.dll");
    if (! hMfplat) hMfplat = LoadLibraryW(L"mfplat.dll");
    if (! hDxgi || ! hD3d11 || ! hMfplat) return false;

    using PfnCreateDXGIFactory1     = HRESULT(WINAPI*)(REFIID, void**);
    using PfnD3D11CreateDevice      = HRESULT(WINAPI*)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT,
                                                  const D3D_FEATURE_LEVEL*, UINT, UINT,
                                                  ID3D11Device**, D3D_FEATURE_LEVEL*,
                                                  ID3D11DeviceContext**);
    using PfnMFCreateDXGIDevManager = HRESULT(WINAPI*)(UINT*, IMFDXGIDeviceManager**);

    auto pfnCreateFactory =
        reinterpret_cast<PfnCreateDXGIFactory1>(GetProcAddress(hDxgi, "CreateDXGIFactory1"));
    auto pfnCreateDevice =
        reinterpret_cast<PfnD3D11CreateDevice>(GetProcAddress(hD3d11, "D3D11CreateDevice"));
    auto pfnCreateMgr = reinterpret_cast<PfnMFCreateDXGIDevManager>(
        GetProcAddress(hMfplat, "MFCreateDXGIDeviceManager"));
    if (! pfnCreateFactory || ! pfnCreateDevice || ! pfnCreateMgr) return false;

    IDXGIFactory1* factory = nullptr;
    if (FAILED(pfnCreateFactory(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory))) ||
        ! factory) {
        return false;
    }

    IDXGIAdapter1* targetAdapter = nullptr;
    for (UINT idx = 0;; ++idx) {
        IDXGIAdapter1* adapter = nullptr;
        if (factory->EnumAdapters1(idx, &adapter) == DXGI_ERROR_NOT_FOUND || ! adapter) break;
        DXGI_ADAPTER_DESC1 desc {};
        if (SUCCEEDED(adapter->GetDesc1(&desc)) &&
            static_cast<int32_t>(desc.AdapterLuid.HighPart) == luidHigh &&
            static_cast<uint32_t>(desc.AdapterLuid.LowPart) == luidLow) {
            targetAdapter = adapter;
            break;
        }
        adapter->Release();
    }
    factory->Release();

    if (! targetAdapter) return false;

    const D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0,
    };
    ID3D11Device*        dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    HRESULT              hr  = pfnCreateDevice(targetAdapter,
                                              D3D_DRIVER_TYPE_UNKNOWN,
                                              nullptr,
                                              D3D11_CREATE_DEVICE_VIDEO_SUPPORT | D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                              levels,
                                              static_cast<UINT>(sizeof(levels) / sizeof(levels[0])),
                                              D3D11_SDK_VERSION,
                                              &dev,
                                              nullptr,
                                              &ctx);
    if (FAILED(hr) || ! dev) {
        hr = pfnCreateDevice(targetAdapter,
                             D3D_DRIVER_TYPE_UNKNOWN,
                             nullptr,
                             D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                             levels,
                             static_cast<UINT>(sizeof(levels) / sizeof(levels[0])),
                             D3D11_SDK_VERSION,
                             &dev,
                             nullptr,
                             &ctx);
    }
    targetAdapter->Release();
    if (ctx) ctx->Release();
    if (FAILED(hr) || ! dev) return false;

    // Enable D3D11 multithread protection required by Media Foundation DXGI device manager.
    ID3D10Multithread* mt = nullptr;
    if (SUCCEEDED(dev->QueryInterface(__uuidof(ID3D10Multithread), reinterpret_cast<void**>(&mt))) &&
        mt) {
        mt->SetMultithreadProtected(TRUE);
        mt->Release();
    }

    UINT                  resetToken = 0;
    IMFDXGIDeviceManager* mgr        = nullptr;
    hr                               = pfnCreateMgr(&resetToken, &mgr);
    if (FAILED(hr) || ! mgr) {
        dev->Release();
        return false;
    }

    hr = mgr->ResetDevice(dev, resetToken);
    if (FAILED(hr)) {
        mgr->Release();
        dev->Release();
        return false;
    }

    *outDevice  = dev;
    *outManager = mgr;
    return true;
}

} // namespace

VideoPlayerWin32::VideoPlayerWin32() = default;

VideoPlayerWin32::~VideoPlayerWin32() { Close(); }

void VideoPlayerWin32::SetFrameConsumer(FrameConsumer consumer) {
    m_consumer = std::move(consumer);
}

void VideoPlayerWin32::SetGpuAdapterLuid(bool enabled, int32_t highPart, uint32_t lowPart) {
    m_useAdapterLuid  = enabled;
    m_adapterLuidHigh = highPart;
    m_adapterLuidLow  = lowPart;
}

bool VideoPlayerWin32::Open(const std::string& videoPathUtf8, uint32_t outWidth, uint32_t outHeight,
                            std::string& outError) {
    Close();

    if (videoPathUtf8.empty() || outWidth == 0 || outHeight == 0) {
        outError = "Invalid video arguments";
        return false;
    }

    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (SUCCEEDED(hr)) {
        m_comInited = true;
    } else if (hr != RPC_E_CHANGED_MODE) {
        outError = "CoInitializeEx failed: " + HexHr(hr);
        return false;
    }

    hr = MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET);
    if (FAILED(hr)) {
        outError = "MFStartup failed: " + HexHr(hr);
        Close();
        return false;
    }
    m_mfInited = true;

    std::wstring widePath = Utf8ToWideVideo(videoPathUtf8);

    auto tryCreateReader = [&](bool withD3dManager) -> HRESULT {
        IMFAttributes* attrs = nullptr;
        HRESULT        aHr   = MFCreateAttributes(&attrs, withD3dManager ? 4 : 2);
        if (SUCCEEDED(aHr) && attrs) {
            if (withD3dManager && m_dxgiManager) {
                attrs->SetUnknown(MF_SOURCE_READER_D3D_MANAGER, m_dxgiManager);
                attrs->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
                attrs->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE);
            } else {
                attrs->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
            }
            attrs->SetUINT32(MF_LOW_LATENCY, TRUE);
        }

        HRESULT rHr = MFCreateSourceReaderFromURL(widePath.c_str(), attrs, &m_reader);
        if (attrs) attrs->Release();
        if (FAILED(rHr) || ! m_reader) return FAILED(rHr) ? rHr : E_FAIL;

        // Deselect all streams except first video stream so audio decoder doesn't stall or waste CPU.
        m_reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE);
        rHr = m_reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), TRUE);
        if (FAILED(rHr)) {
            m_reader->Release();
            m_reader = nullptr;
            return rHr;
        }

        // Query native frame size so we can request hardware/MFT pre-downscale for 2K/4K sources.
        UINT32 nativeW = 0, nativeH = 0;
        {
            IMFMediaType* nativeType = nullptr;
            if (SUCCEEDED(m_reader->GetNativeMediaType(
                    static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0, &nativeType)) &&
                nativeType) {
                MFGetAttributeSize(nativeType, MF_MT_FRAME_SIZE, &nativeW, &nativeH);
                nativeType->Release();
            }
        }

        // Request RGB32 (BGRA32 in memory on little-endian Windows).
        // For high-res 2K/4K videos on small LCDs, ask Media Foundation's Video Processor MFT
        // to pre-scale down to an aspect-preserving size (<= max(960, max(outW, outH))) so we
        // avoid reading back 33 MB/frame 4K buffers over the memory bus every frame.
        const uint32_t preScaleCap = std::max<uint32_t>(960u, std::max(outWidth, outHeight));
        bool           configured  = false;
        if (nativeW > 0 && nativeH > 0 && (nativeW > preScaleCap || nativeH > preScaleCap)) {
            double scale  = static_cast<double>(preScaleCap) /
                            static_cast<double>(std::max(nativeW, nativeH));
            uint32_t decW = std::max<uint32_t>(64u, static_cast<uint32_t>(std::lround(nativeW * scale)) & ~1u);
            uint32_t decH = std::max<uint32_t>(64u, static_cast<uint32_t>(std::lround(nativeH * scale)) & ~1u);
            IMFMediaType* scaledType = nullptr;
            if (SUCCEEDED(MFCreateMediaType(&scaledType)) && scaledType) {
                scaledType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
                scaledType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
                MFSetAttributeSize(scaledType, MF_MT_FRAME_SIZE, decW, decH);
                if (SUCCEEDED(m_reader->SetCurrentMediaType(
                        static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), nullptr, scaledType))) {
                    configured = true;
                }
                scaledType->Release();
            }
        }

        if (! configured) {
            IMFMediaType* outType = nullptr;
            rHr                   = MFCreateMediaType(&outType);
            if (SUCCEEDED(rHr) && outType) {
                outType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
                outType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
                rHr = m_reader->SetCurrentMediaType(
                    static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), nullptr, outType);
                outType->Release();
            }
            if (FAILED(rHr)) {
                m_reader->Release();
                m_reader = nullptr;
                return rHr;
            }
        }
        return S_OK;
    };

    bool usedD3d = false;
    if (m_useAdapterLuid) {
        if (TryCreateDxgiDeviceManagerForLuid(m_adapterLuidHigh, m_adapterLuidLow, &m_d3dDevice, &m_dxgiManager)) {
            hr = tryCreateReader(true);
            if (SUCCEEDED(hr)) {
                usedD3d = true;
            } else {
                if (m_dxgiManager) {
                    m_dxgiManager->Release();
                    m_dxgiManager = nullptr;
                }
                if (m_d3dDevice) {
                    m_d3dDevice->Release();
                    m_d3dDevice = nullptr;
                }
            }
        }
    }

    if (! usedD3d) {
        hr = tryCreateReader(false);
    }

    if (FAILED(hr) || ! m_reader) {
        outError = "Media Foundation failed to open/configure video (" + HexHr(hr) + "): " + videoPathUtf8;
        Close();
        return false;
    }

    IMFMediaType* currentType = nullptr;
    hr = m_reader->GetCurrentMediaType(
        static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), &currentType);
    if (FAILED(hr) || ! currentType) {
        outError = "Failed to query decoded video media type (" + HexHr(hr) + ")";
        Close();
        return false;
    }

    UINT32 w = 0, h = 0;
    MFGetAttributeSize(currentType, MF_MT_FRAME_SIZE, &w, &h);
    UINT32 fpsNum = 30, fpsDen = 1;
    if (SUCCEEDED(MFGetAttributeRatio(currentType, MF_MT_FRAME_RATE, &fpsNum, &fpsDen)) &&
        fpsNum > 0 && fpsDen > 0) {
        double fps              = static_cast<double>(fpsNum) / static_cast<double>(fpsDen);
        fps                     = std::clamp(fps, 1.0, 120.0);
        m_videoFrameDurationSec = 1.0 / fps;
    } else {
        m_videoFrameDurationSec = 1.0 / 30.0;
    }

    UINT32 rawStride = 0;
    if (SUCCEEDED(currentType->GetUINT32(MF_MT_DEFAULT_STRIDE, &rawStride)) && rawStride != 0) {
        m_srcStride = static_cast<int32_t>(rawStride);
    } else {
        LONG calcStride = 0;
        if (SUCCEEDED(MFGetStrideForBitmapInfoHeader(MFVideoFormat_RGB32.Data1, w, &calcStride)) &&
            calcStride != 0) {
            m_srcStride = static_cast<int32_t>(calcStride);
        } else {
            m_srcStride = static_cast<int32_t>(w * 4u);
        }
    }
    currentType->Release();

    if (w == 0 || h == 0) {
        outError = "Decoded video has invalid dimensions";
        Close();
        return false;
    }

    m_srcWidth        = w;
    m_srcHeight       = h;
    m_outWidth        = outWidth;
    m_outHeight       = outHeight;
    m_accumSec        = 0.0;
    m_hasDecodedFirst = false;
    m_outBgra.resize(static_cast<std::size_t>(m_outWidth) * m_outHeight * 4u);
    return true;
}

void VideoPlayerWin32::SetFitMode(int fitMode) {
    int clamped = (fitMode == 1 || fitMode == 2) ? fitMode : 0;
    if (m_fitMode == clamped) return;
    m_fitMode = clamped;
    if (m_reader && ! m_lastSrcBgra.empty() && m_lastSrcW > 0 && m_lastSrcH > 0) {
        BlitToOutput(m_lastSrcBgra.data(), m_lastSrcW, m_lastSrcH, m_lastSrcStride);
        if (m_consumer) {
            m_consumer(m_outBgra.data(), m_outWidth, m_outHeight, m_outWidth * 4u);
        }
    }
}

bool VideoPlayerWin32::Resize(uint32_t outWidth, uint32_t outHeight) {
    if (! m_reader || outWidth == 0 || outHeight == 0) return false;
    m_outWidth  = outWidth;
    m_outHeight = outHeight;
    m_outBgra.resize(static_cast<std::size_t>(m_outWidth) * m_outHeight * 4u);

    // Re-scale last decoded frame immediately so resize produces crisp pixels right away.
    if (! m_lastSrcBgra.empty() && m_lastSrcW > 0 && m_lastSrcH > 0) {
        BlitToOutput(m_lastSrcBgra.data(), m_lastSrcW, m_lastSrcH, m_lastSrcStride);
        if (m_consumer) {
            m_consumer(m_outBgra.data(), m_outWidth, m_outHeight, m_outWidth * 4u);
        }
    }
    return true;
}

bool VideoPlayerWin32::RenderFrame(double deltaSec) {
    if (! m_reader) return false;

    if (! m_hasDecodedFirst) {
        if (! DecodeNextFrame(true)) return false;
        m_hasDecodedFirst = true;
        return true;
    }

    m_accumSec += std::clamp(deltaSec, 0.0, 0.25);
    // Skip intermediate catch-up frames without CPU pixel copy/scaling (e.g. 60fps video at 15-30fps).
    const int maxCatchup     = m_outputRgba ? 2 : 4;
    int       framesToDecode = 0;
    while (m_accumSec >= m_videoFrameDurationSec && framesToDecode < maxCatchup) {
        m_accumSec -= m_videoFrameDurationSec;
        ++framesToDecode;
    }
    if (m_accumSec > m_videoFrameDurationSec * 2.0) {
        m_accumSec = 0.0;
    }

    if (framesToDecode == 0) {
        return true; // No new video frame due yet; SHM already holds the current frame.
    }

    for (int i = 0; i < framesToDecode; ++i) {
        const bool isFinal = (i + 1 == framesToDecode);
        if (! DecodeNextFrame(isFinal)) return false;
    }
    return true;
}

bool VideoPlayerWin32::DecodeNextFrame(bool convertAndPublish) {
    if (! m_reader) return false;

    for (int tries = 0; tries < 6; ++tries) {
        DWORD      streamIndex = 0;
        DWORD      flags       = 0;
        LONGLONG   timestamp   = 0;
        IMFSample* sample      = nullptr;

        HRESULT hr = m_reader->ReadSample(
            static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM),
            0,
            &streamIndex,
            &flags,
            &timestamp,
            &sample);
        if (FAILED(hr)) {
            if (sample) sample->Release();
            return false;
        }

        if ((flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) != 0) {
            IMFMediaType* cur = nullptr;
            if (SUCCEEDED(m_reader->GetCurrentMediaType(
                    static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), &cur)) &&
                cur) {
                UINT32 w = 0, h = 0;
                if (SUCCEEDED(MFGetAttributeSize(cur, MF_MT_FRAME_SIZE, &w, &h)) && w > 0 && h > 0) {
                    m_srcWidth  = w;
                    m_srcHeight = h;
                }
                UINT32 rawStride = 0;
                if (SUCCEEDED(cur->GetUINT32(MF_MT_DEFAULT_STRIDE, &rawStride)) && rawStride != 0) {
                    m_srcStride = static_cast<int32_t>(rawStride);
                } else {
                    m_srcStride = static_cast<int32_t>(m_srcWidth * 4u);
                }
                cur->Release();
            }
        }

        if ((flags & MF_SOURCE_READERF_ENDOFSTREAM) != 0) {
            if (sample) sample->Release();
            // Seamless loop back to start of video
            PROPVARIANT var;
            PropVariantInit(&var);
            var.vt            = VT_I8;
            var.hVal.QuadPart = 0;
            HRESULT seekHr    = m_reader->SetCurrentPosition(GUID_NULL, var);
            PropVariantClear(&var);
            if (FAILED(seekHr)) return false;
            continue;
        }

        if (! sample) continue;

        if (! convertAndPublish) {
            sample->Release();
            return true;
        }

        IMFMediaBuffer* buffer = nullptr;
        hr                     = sample->ConvertToContiguousBuffer(&buffer);
        sample->Release();
        if (FAILED(hr) || ! buffer) continue;

        BYTE* data   = nullptr;
        DWORD maxLen = 0, curLen = 0;
        hr = buffer->Lock(&data, &maxLen, &curLen);
        if (SUCCEEDED(hr) && data != nullptr && curLen > 0) {
            const std::size_t absStride = static_cast<std::size_t>(std::abs(m_srcStride));
            const std::size_t needed    = absStride * m_srcHeight;
            if (curLen >= needed && m_srcWidth > 0 && m_srcHeight > 0) {
                // Only cache last raw source frame for immediate resize repaint when <= 720p;
                // avoids copying 33 MB/frame on 4K fallback paths.
                if (! m_outputRgba &&
                    static_cast<uint64_t>(m_srcWidth) * static_cast<uint64_t>(m_srcHeight) <=
                        1280ULL * 720ULL) {
                    m_lastSrcW      = m_srcWidth;
                    m_lastSrcH      = m_srcHeight;
                    m_lastSrcStride = m_srcStride;
                    m_lastSrcBgra.assign(data, data + needed);
                } else if (! m_lastSrcBgra.empty()) {
                    m_lastSrcBgra.clear();
                    m_lastSrcW = 0;
                    m_lastSrcH = 0;
                }
                BlitToOutput(data, m_srcWidth, m_srcHeight, m_srcStride);
                if (m_consumer) {
                    m_consumer(m_outBgra.data(), m_outWidth, m_outHeight, m_outWidth * 4u);
                }
                buffer->Unlock();
                buffer->Release();
                return true;
            }
            buffer->Unlock();
        }
        buffer->Release();
    }
    return false;
}

void VideoPlayerWin32::BlitToOutput(const uint8_t* srcBgra, uint32_t srcW, uint32_t srcH,
                                    int32_t srcStride) {
    if (m_outWidth == 0 || m_outHeight == 0 || srcW == 0 || srcH == 0) return;
    const bool        bottomUp  = (srcStride < 0);
    const std::size_t absStride = static_cast<std::size_t>(std::abs(srcStride));

    if (m_lutSrcW != srcW || m_lutSrcH != srcH || m_lutOutW != m_outWidth ||
        m_lutOutH != m_outHeight || m_lutFitMode != m_fitMode) {
        const double srcAspect = static_cast<double>(srcW) / static_cast<double>(srcH);
        const double dstAspect = static_cast<double>(m_outWidth) / static_cast<double>(m_outHeight);

        double   cropX = 0.0, cropY = 0.0;
        double   cropW = static_cast<double>(srcW);
        double   cropH = static_cast<double>(srcH);
        uint32_t dstX0 = 0, dstY0 = 0;
        uint32_t dstW = m_outWidth, dstH = m_outHeight;

        if (m_fitMode == 1) {
            if (srcAspect > dstAspect) {
                dstH  = std::max<uint32_t>(1, static_cast<uint32_t>(std::lround(m_outWidth / srcAspect)));
                dstH  = std::min(dstH, m_outHeight);
                dstY0 = (m_outHeight - dstH) / 2u;
            } else {
                dstW  = std::max<uint32_t>(1, static_cast<uint32_t>(std::lround(m_outHeight * srcAspect)));
                dstW  = std::min(dstW, m_outWidth);
                dstX0 = (m_outWidth - dstW) / 2u;
            }
        } else if (m_fitMode == 0) {
            if (srcAspect > dstAspect) {
                cropW = static_cast<double>(srcH) * dstAspect;
                cropX = (static_cast<double>(srcW) - cropW) * 0.5;
            } else {
                cropH = static_cast<double>(srcW) / dstAspect;
                cropY = (static_cast<double>(srcH) - cropH) * 0.5;
            }
        }

        const double stepX = cropW / static_cast<double>(dstW);
        const double stepY = cropH / static_cast<double>(dstH);

        m_lutX0.resize(dstW);
        m_lutX1.resize(dstW);
        m_lutWx.resize(dstW);
        for (uint32_t dx = 0; dx < dstW; ++dx) {
            double sx   = cropX + (static_cast<double>(dx) + 0.5) * stepX - 0.5;
            sx          = std::clamp(sx, 0.0, static_cast<double>(srcW - 1));
            uint32_t i0 = static_cast<uint32_t>(sx);
            uint32_t i1 = std::min(i0 + 1, srcW - 1);
            m_lutX0[dx] = i0 * 4u;
            m_lutX1[dx] = i1 * 4u;
            m_lutWx[dx] = static_cast<uint32_t>((sx - static_cast<double>(i0)) * 256.0);
        }

        m_lutSrcW    = srcW;
        m_lutSrcH    = srcH;
        m_lutOutW    = m_outWidth;
        m_lutOutH    = m_outHeight;
        m_lutFitMode = m_fitMode;
        m_lutCropY   = cropY;
        m_lutStepY   = stepY;
        m_lutDstX0   = dstX0;
        m_lutDstY0   = dstY0;
        m_lutDstW    = dstW;
        m_lutDstH    = dstH;

        if (m_fitMode == 1) {
            // Clear letterbox / pillarbox bars once on geometry change
            std::memset(m_outBgra.data(), 0, m_outBgra.size());
            for (std::size_t i = 3; i < m_outBgra.size(); i += 4) m_outBgra[i] = 0xFF;
        }
    }

    const int       ch0   = m_outputRgba ? 2 : 0;
    const int       ch2   = m_outputRgba ? 0 : 2;
    const uint32_t  dstX0 = m_lutDstX0;
    const uint32_t  dstY0 = m_lutDstY0;
    const uint32_t  dstW  = m_lutDstW;
    const uint32_t  dstH  = m_lutDstH;
    const uint32_t* x0Arr = m_lutX0.data();
    const uint32_t* x1Arr = m_lutX1.data();
    const uint32_t* wxArr = m_lutWx.data();

    for (uint32_t dy = 0; dy < dstH; ++dy) {
        double sy    = m_lutCropY + (static_cast<double>(dy) + 0.5) * m_lutStepY - 0.5;
        sy           = std::clamp(sy, 0.0, static_cast<double>(srcH - 1));
        uint32_t y0  = static_cast<uint32_t>(sy);
        uint32_t y1  = std::min(y0 + 1, srcH - 1);
        uint32_t wy  = static_cast<uint32_t>((sy - static_cast<double>(y0)) * 256.0);
        uint32_t iwy = 256u - wy;

        uint32_t       rowIdx0 = bottomUp ? (srcH - 1 - y0) : y0;
        uint32_t       rowIdx1 = bottomUp ? (srcH - 1 - y1) : y1;
        const uint8_t* r0      = srcBgra + static_cast<std::size_t>(rowIdx0) * absStride;
        const uint8_t* r1      = srcBgra + static_cast<std::size_t>(rowIdx1) * absStride;
        uint8_t*       dst     = m_outBgra.data() +
                           (static_cast<std::size_t>(dstY0 + dy) * m_outWidth + dstX0) * 4u;

        for (uint32_t dx = 0; dx < dstW; ++dx) {
            uint32_t ox0 = x0Arr[dx];
            uint32_t ox1 = x1Arr[dx];
            uint32_t wx  = wxArr[dx];
            uint32_t iwx = 256u - wx;

            uint32_t w00 = iwx * iwy;
            uint32_t w10 = wx * iwy;
            uint32_t w01 = iwx * wy;
            uint32_t w11 = wx * wy;

            dst[dx * 4 + 0] = static_cast<uint8_t>((r0[ox0 + ch0] * w00 + r0[ox1 + ch0] * w10 +
                                                    r1[ox0 + ch0] * w01 + r1[ox1 + ch0] * w11) >> 16);
            dst[dx * 4 + 1] = static_cast<uint8_t>((r0[ox0 + 1] * w00 + r0[ox1 + 1] * w10 +
                                                    r1[ox0 + 1] * w01 + r1[ox1 + 1] * w11) >> 16);
            dst[dx * 4 + 2] = static_cast<uint8_t>((r0[ox0 + ch2] * w00 + r0[ox1 + ch2] * w10 +
                                                    r1[ox0 + ch2] * w01 + r1[ox1 + ch2] * w11) >> 16);
            dst[dx * 4 + 3] = 0xFF;
        }
    }
}

void VideoPlayerWin32::Close() {
    if (m_reader) {
        m_reader->Release();
        m_reader = nullptr;
    }
    if (m_dxgiManager) {
        m_dxgiManager->Release();
        m_dxgiManager = nullptr;
    }
    if (m_d3dDevice) {
        m_d3dDevice->Release();
        m_d3dDevice = nullptr;
    }
    if (m_mfInited) {
        MFShutdown();
        m_mfInited = false;
    }
    if (m_comInited) {
        CoUninitialize();
        m_comInited = false;
    }
    m_srcWidth        = 0;
    m_srcHeight       = 0;
    m_srcStride       = 0;
    m_hasDecodedFirst = false;
    m_accumSec        = 0.0;
    m_outBgra.clear();
    m_lastSrcBgra.clear();
    m_lutSrcW    = 0;
    m_lutSrcH    = 0;
    m_lutOutW    = 0;
    m_lutOutH    = 0;
    m_lutFitMode = -1;
}

} // namespace lcdfusion_wpe

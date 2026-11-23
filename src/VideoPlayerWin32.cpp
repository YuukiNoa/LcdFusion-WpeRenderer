#include "VideoPlayerWin32.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <objbase.h>
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

} // namespace

VideoPlayerWin32::VideoPlayerWin32() = default;

VideoPlayerWin32::~VideoPlayerWin32() { Close(); }

void VideoPlayerWin32::SetFrameConsumer(FrameConsumer consumer) {
    m_consumer = std::move(consumer);
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

    IMFAttributes* attrs = nullptr;
    hr                   = MFCreateAttributes(&attrs, 2);
    if (SUCCEEDED(hr) && attrs) {
        attrs->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
        attrs->SetUINT32(MF_LOW_LATENCY, TRUE);
    }

    std::wstring widePath = Utf8ToWideVideo(videoPathUtf8);
    hr = MFCreateSourceReaderFromURL(widePath.c_str(), attrs, &m_reader);
    if (attrs) attrs->Release();

    if (FAILED(hr) || ! m_reader) {
        outError = "Media Foundation failed to open video (" + HexHr(hr) + "): " + videoPathUtf8;
        Close();
        return false;
    }

    // Deselect all streams except first video stream so audio decoder doesn't stall or waste CPU.
    m_reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE);
    hr = m_reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), TRUE);
    if (FAILED(hr)) {
        outError = "Video file has no decodable video stream (" + HexHr(hr) + ")";
        Close();
        return false;
    }

    // Request RGB32 (BGRA32 in memory on little-endian Windows).
    IMFMediaType* outType = nullptr;
    hr                    = MFCreateMediaType(&outType);
    if (SUCCEEDED(hr) && outType) {
        outType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        outType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
        hr = m_reader->SetCurrentMediaType(
            static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), nullptr, outType);
        outType->Release();
    }
    if (FAILED(hr)) {
        outError = "Failed to negotiate MFVideoFormat_RGB32 decoder output (" + HexHr(hr) + ")";
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
        if (! DecodeNextFrame()) return false;
        m_hasDecodedFirst = true;
        return true;
    }

    m_accumSec += std::clamp(deltaSec, 0.0, 0.25);
    int decoded = 0;
    // Catch up at most 1 video frame per render tick for high-res textures (2 for standalone video).
    const int maxCatchup = m_outputRgba ? 1 : 2;
    while (m_accumSec >= m_videoFrameDurationSec && decoded < maxCatchup) {
        if (! DecodeNextFrame()) return false;
        m_accumSec -= m_videoFrameDurationSec;
        ++decoded;
    }
    if (m_accumSec > m_videoFrameDurationSec * 2.0) {
        m_accumSec = 0.0;
    }

    if (decoded == 0 && ! m_outBgra.empty() && m_consumer) {
        m_consumer(m_outBgra.data(), m_outWidth, m_outHeight, m_outWidth * 4u);
    }
    return true;
}

bool VideoPlayerWin32::DecodeNextFrame() {
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
                if (! m_outputRgba) {
                    m_lastSrcW      = m_srcWidth;
                    m_lastSrcH      = m_srcHeight;
                    m_lastSrcStride = m_srcStride;
                    m_lastSrcBgra.assign(data, data + needed);
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

    const double srcAspect = static_cast<double>(srcW) / static_cast<double>(srcH);
    const double dstAspect = static_cast<double>(m_outWidth) / static_cast<double>(m_outHeight);

    double   cropX = 0.0, cropY = 0.0;
    double   cropW = static_cast<double>(srcW);
    double   cropH = static_cast<double>(srcH);
    uint32_t dstX0 = 0, dstY0 = 0;
    uint32_t dstW = m_outWidth, dstH = m_outHeight;

    if (m_fitMode == 1) {
        // Fit (letterbox / pillarbox with opaque black background)
        std::memset(m_outBgra.data(), 0, m_outBgra.size());
        for (std::size_t i = 3; i < m_outBgra.size(); i += 4) m_outBgra[i] = 0xFF;
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
        // Fill (ASPECTCROP center-crop)
        if (srcAspect > dstAspect) {
            cropW = static_cast<double>(srcH) * dstAspect;
            cropX = (static_cast<double>(srcW) - cropW) * 0.5;
        } else {
            cropH = static_cast<double>(srcW) / dstAspect;
            cropY = (static_cast<double>(srcH) - cropH) * 0.5;
        }
    }
    // m_fitMode == 2 (Stretch): full srcW x srcH -> full m_outWidth x m_outHeight

    const double stepX = cropW / static_cast<double>(dstW);
    const double stepY = cropH / static_cast<double>(dstH);

    // Precompute X lookup indices & bilinear weights
    std::vector<uint32_t> x0Arr(dstW), x1Arr(dstW);
    std::vector<uint32_t> wxArr(dstW);
    for (uint32_t dx = 0; dx < dstW; ++dx) {
        double sx = cropX + (static_cast<double>(dx) + 0.5) * stepX - 0.5;
        sx        = std::clamp(sx, 0.0, static_cast<double>(srcW - 1));
        uint32_t i0 = static_cast<uint32_t>(sx);
        uint32_t i1 = std::min(i0 + 1, srcW - 1);
        x0Arr[dx]   = i0 * 4u;
        x1Arr[dx]   = i1 * 4u;
        wxArr[dx]   = static_cast<uint32_t>((sx - static_cast<double>(i0)) * 256.0);
    }

    const int ch0 = m_outputRgba ? 2 : 0;
    const int ch2 = m_outputRgba ? 0 : 2;

    for (uint32_t dy = 0; dy < dstH; ++dy) {
        double sy = cropY + (static_cast<double>(dy) + 0.5) * stepY - 0.5;
        sy        = std::clamp(sy, 0.0, static_cast<double>(srcH - 1));
        uint32_t y0 = static_cast<uint32_t>(sy);
        uint32_t y1 = std::min(y0 + 1, srcH - 1);
        uint32_t wy = static_cast<uint32_t>((sy - static_cast<double>(y0)) * 256.0);
        uint32_t iwy = 256u - wy;

        uint32_t rowIdx0 = bottomUp ? (srcH - 1 - y0) : y0;
        uint32_t rowIdx1 = bottomUp ? (srcH - 1 - y1) : y1;
        const uint8_t* r0 = srcBgra + static_cast<std::size_t>(rowIdx0) * absStride;
        const uint8_t* r1 = srcBgra + static_cast<std::size_t>(rowIdx1) * absStride;
        uint8_t*       dst = m_outBgra.data() +
                       (static_cast<std::size_t>(dstY0 + dy) * m_outWidth + dstX0) * 4u;

        for (uint32_t dx = 0; dx < dstW; ++dx) {
            uint32_t       ox0 = x0Arr[dx];
            uint32_t       ox1 = x1Arr[dx];
            uint32_t       wx  = wxArr[dx];
            uint32_t       iwx = 256u - wx;

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
}

} // namespace lcdfusion_wpe

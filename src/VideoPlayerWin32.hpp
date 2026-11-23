#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

struct IMFSourceReader;
struct IMFDXGIDeviceManager;
struct ID3D11Device;

namespace lcdfusion_wpe
{

// Decodes Video wallpapers (MP4/WMV/MOV/etc.) on x64 using Windows Media Foundation IMFSourceReader
// and outputs ASPECTCROP-scaled BGRA32 frames directly into the shared v1 FrameConsumer pipeline.
class VideoPlayerWin32 {
public:
    using FrameConsumer =
        std::function<void(const uint8_t* bgra, uint32_t width, uint32_t height, uint32_t stride)>;

    VideoPlayerWin32();
    ~VideoPlayerWin32();

    VideoPlayerWin32(const VideoPlayerWin32&)            = delete;
    VideoPlayerWin32& operator=(const VideoPlayerWin32&) = delete;

    void SetFrameConsumer(FrameConsumer consumer);
    // 0 = Fill (AspectCrop), 1 = Fit (Letterbox), 2 = Stretch
    void SetFitMode(int fitMode);
    // false = BGRA32 (IPC output), true = RGBA32 (Vulkan texture upload)
    void SetOutputRgba(bool rgba) { m_outputRgba = rgba; }
    // Optional: bind Media Foundation hardware decoding to a specific Windows GPU adapter LUID.
    void SetGpuAdapterLuid(bool enabled, int32_t highPart, uint32_t lowPart);

    bool Open(const std::string& videoPathUtf8, uint32_t outWidth, uint32_t outHeight,
              std::string& outError);
    bool Resize(uint32_t outWidth, uint32_t outHeight);
    bool RenderFrame(double deltaSec);
    void Close();

    bool     IsOpen() const { return m_reader != nullptr; }
    uint32_t Width() const { return m_outWidth; }
    uint32_t Height() const { return m_outHeight; }

private:
    bool DecodeNextFrame(bool convertAndPublish);
    void BlitToOutput(const uint8_t* srcBgra, uint32_t srcW, uint32_t srcH, int32_t srcStride);

    bool                  m_comInited { false };
    bool                  m_mfInited { false };
    IMFSourceReader*      m_reader { nullptr };
    IMFDXGIDeviceManager* m_dxgiManager { nullptr };
    ID3D11Device*         m_d3dDevice { nullptr };

    bool     m_useAdapterLuid { false };
    int32_t  m_adapterLuidHigh { 0 };
    uint32_t m_adapterLuidLow { 0 };

    int      m_fitMode { 0 };
    bool     m_outputRgba { false };
    uint32_t m_srcWidth { 0 };
    uint32_t m_srcHeight { 0 };
    int32_t  m_srcStride { 0 };
    double   m_videoFrameDurationSec { 1.0 / 30.0 };
    double   m_accumSec { 0.0 };
    bool     m_hasDecodedFirst { false };

    uint32_t              m_outWidth { 0 };
    uint32_t              m_outHeight { 0 };
    std::vector<uint8_t>  m_outBgra;
    std::vector<uint8_t>  m_lastSrcBgra;
    uint32_t              m_lastSrcW { 0 };
    uint32_t              m_lastSrcH { 0 };
    int32_t               m_lastSrcStride { 0 };

    // Cached bilinear X lookup table & layout geometry (rebuilt only on size/fit change)
    uint32_t              m_lutSrcW { 0 };
    uint32_t              m_lutSrcH { 0 };
    uint32_t              m_lutOutW { 0 };
    uint32_t              m_lutOutH { 0 };
    int                   m_lutFitMode { -1 };
    double                m_lutCropY { 0.0 };
    double                m_lutStepY { 0.0 };
    uint32_t              m_lutDstX0 { 0 };
    uint32_t              m_lutDstY0 { 0 };
    uint32_t              m_lutDstW { 0 };
    uint32_t              m_lutDstH { 0 };
    std::vector<uint32_t> m_lutX0;
    std::vector<uint32_t> m_lutX1;
    std::vector<uint32_t> m_lutWx;

    FrameConsumer m_consumer;
};

} // namespace lcdfusion_wpe

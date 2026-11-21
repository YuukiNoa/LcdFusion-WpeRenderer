#include "AudioSpectrumWin32.hpp"
#include "Utils/Logging.h"

#ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <objbase.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <mmreg.h>

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <vector>

namespace lcdfusion_wpe
{

namespace
{

constexpr double kPi = 3.14159265358979323846;

const GUID kSubformatIeeeFloat = {
    0x00000003, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 }
};
const GUID kSubformatPcm = {
    0x00000001, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 }
};

void Radix2Fft(std::array<std::complex<float>, 1024>& a) {
    constexpr std::size_t N = 1024;
    // Bit-reversal permutation
    for (std::size_t i = 1, j = 0; i < N; ++i) {
        std::size_t bit = N >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (std::size_t len = 2; len <= N; len <<= 1) {
        double angle = -2.0 * kPi / static_cast<double>(len);
        std::complex<float> wlen(static_cast<float>(std::cos(angle)),
                                 static_cast<float>(std::sin(angle)));
        for (std::size_t i = 0; i < N; i += len) {
            std::complex<float> w(1.0f, 0.0f);
            for (std::size_t j = 0; j < len / 2; ++j) {
                auto u             = a[i + j];
                auto v             = a[i + j + len / 2] * w;
                a[i + j]           = u + v;
                a[i + j + len / 2] = u - v;
                w *= wlen;
            }
        }
    }
}

bool IsFloatFormat(const WAVEFORMATEX* wf) {
    if (! wf) return false;
    if (wf->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) return true;
    if (wf->wFormatTag == WAVE_FORMAT_EXTENSIBLE && wf->cbSize >= 22) {
        const auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(wf);
        return IsEqualGUID(ext->SubFormat, kSubformatIeeeFloat) != 0;
    }
    return false;
}

bool IsPcm16Format(const WAVEFORMATEX* wf) {
    if (! wf || wf->wBitsPerSample != 16) return false;
    if (wf->wFormatTag == WAVE_FORMAT_PCM) return true;
    if (wf->wFormatTag == WAVE_FORMAT_EXTENSIBLE && wf->cbSize >= 22) {
        const auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(wf);
        return IsEqualGUID(ext->SubFormat, kSubformatPcm) != 0;
    }
    return false;
}

} // namespace

WasapiAudioAnalyzer::WasapiAudioAnalyzer() = default;

WasapiAudioAnalyzer::~WasapiAudioAnalyzer() { Stop(); }

void WasapiAudioAnalyzer::Start() {
    bool expected = false;
    if (! m_running.compare_exchange_strong(expected, true)) return;
    m_thread = std::thread(&WasapiAudioAnalyzer::CaptureLoop, this);
}

void WasapiAudioAnalyzer::Stop() {
    m_running.store(false);
    if (m_thread.joinable()) {
        m_thread.join();
    }
}

void WasapiAudioAnalyzer::SetPaused(bool paused) { m_paused.store(paused); }

void WasapiAudioAnalyzer::GetSpectrum(AudioSpectrumData& out) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    out = m_spectrum;
}

void WasapiAudioAnalyzer::FeedPcmSamples(const float* left, const float* right,
                                         std::size_t frameCount) {
    if (! left || ! right || frameCount == 0) return;
    PushSamplesAndAnalyze(left, right, frameCount);
}

void WasapiAudioAnalyzer::PushSamplesAndAnalyze(const float* left, const float* right,
                                                std::size_t frameCount) {
    for (std::size_t i = 0; i < frameCount; ++i) {
        m_ringL[m_ringPos] = left[i];
        m_ringR[m_ringPos] = right[i];
        m_ringPos          = (m_ringPos + 1) % kFftSize;
    }
    ComputeBandsFromWindow();
}

void WasapiAudioAnalyzer::ComputeBandsFromWindow() {
    std::array<std::complex<float>, kFftSize> fftL {};
    std::array<std::complex<float>, kFftSize> fftR {};

    for (std::size_t i = 0; i < kFftSize; ++i) {
        std::size_t idx = (m_ringPos + i) % kFftSize;
        float       w   = 0.5f * (1.0f - std::cos(static_cast<float>(2.0 * kPi * i / (kFftSize - 1))));
        fftL[i]         = std::complex<float>(m_ringL[idx] * w, 0.0f);
        fftR[i]         = std::complex<float>(m_ringR[idx] * w, 0.0f);
    }

    Radix2Fft(fftL);
    Radix2Fft(fftR);

    std::array<float, 64> rawL {};
    std::array<float, 64> rawR {};

    // Logarithmic perceptual mapping (~45 Hz .. ~16 kHz) across 64 bands so bass (45-350 Hz)
    // maps cleanly into low bands [0..15] matching Wallpaper Engine visualizers.
    constexpr float kNorm     = 1.0f / static_cast<float>(kFftSize * 0.25f);
    const double    sr        = static_cast<double>(m_sampleRate > 0 ? m_sampleRate : 48000);
    const double    minHz     = 45.0;
    const double    maxHz     = std::min(16000.0, sr * 0.45);
    const double    logRatio  = std::log(maxHz / minHz);
    const std::size_t maxBin  = kFftSize / 2 - 1;

    for (std::size_t b = 0; b < 64; ++b) {
        double f0 = minHz * std::exp(logRatio * (static_cast<double>(b) / 64.0));
        double f1 = minHz * std::exp(logRatio * (static_cast<double>(b + 1) / 64.0));
        std::size_t bin0 = std::clamp<std::size_t>(
            static_cast<std::size_t>(std::floor(f0 * kFftSize / sr)), 1, maxBin);
        std::size_t bin1 = std::clamp<std::size_t>(
            static_cast<std::size_t>(std::ceil(f1 * kFftSize / sr)), bin0, maxBin);

        float peakL = 0.0f;
        float peakR = 0.0f;
        float sumL  = 0.0f;
        float sumR  = 0.0f;
        std::size_t cnt = 0;
        for (std::size_t k = bin0; k <= bin1; ++k) {
            float mL = std::abs(fftL[k]) * kNorm;
            float mR = std::abs(fftR[k]) * kNorm;
            peakL    = std::max(peakL, mL);
            peakR    = std::max(peakR, mR);
            sumL += mL;
            sumR += mR;
            ++cnt;
        }
        float magL  = 0.65f * peakL + 0.35f * (sumL / static_cast<float>(std::max<std::size_t>(1, cnt)));
        float magR  = 0.65f * peakR + 0.35f * (sumR / static_cast<float>(std::max<std::size_t>(1, cnt)));
        float boost = 1.15f + 0.85f * (static_cast<float>(b) / 63.0f);
        rawL[b]     = std::clamp(std::sqrt(magL * boost), 0.0f, 1.5f);
        rawR[b]     = std::clamp(std::sqrt(magR * boost), 0.0f, 1.5f);
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    for (std::size_t b = 0; b < 64; ++b) {
        float alphaL          = (rawL[b] > m_spectrum.left64[b]) ? 0.50f : 0.20f;
        float alphaR          = (rawR[b] > m_spectrum.right64[b]) ? 0.50f : 0.20f;
        m_spectrum.left64[b]  = m_spectrum.left64[b] * (1.0f - alphaL) + rawL[b] * alphaL;
        m_spectrum.right64[b] = m_spectrum.right64[b] * (1.0f - alphaR) + rawR[b] * alphaR;
    }
    for (std::size_t b = 0; b < 32; ++b) {
        m_spectrum.left32[b]  = 0.5f * (m_spectrum.left64[b * 2] + m_spectrum.left64[b * 2 + 1]);
        m_spectrum.right32[b] = 0.5f * (m_spectrum.right64[b * 2] + m_spectrum.right64[b * 2 + 1]);
    }
    for (std::size_t b = 0; b < 16; ++b) {
        m_spectrum.left16[b]  = 0.5f * (m_spectrum.left32[b * 2] + m_spectrum.left32[b * 2 + 1]);
        m_spectrum.right16[b] = 0.5f * (m_spectrum.right32[b * 2] + m_spectrum.right32[b * 2 + 1]);
    }
}

void WasapiAudioAnalyzer::CaptureLoop() {
    HRESULT hrCom = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    bool    comOk = SUCCEEDED(hrCom);

    std::vector<float> tmpL;
    std::vector<float> tmpR;
    const std::array<float, 64> zeros {};

    while (m_running.load()) {
        if (m_paused.load()) {
            Sleep(50);
            continue;
        }

        IMMDeviceEnumerator* enumerator = nullptr;
        IMMDevice*           device     = nullptr;
        IAudioClient*        client     = nullptr;
        IAudioCaptureClient* capture    = nullptr;
        WAVEFORMATEX*        wfex       = nullptr;

        auto cleanup = [&]() {
            if (client) client->Stop();
            if (capture) { capture->Release(); capture = nullptr; }
            if (client) { client->Release(); client = nullptr; }
            if (wfex) { CoTaskMemFree(wfex); wfex = nullptr; }
            if (device) { device->Release(); device = nullptr; }
            if (enumerator) { enumerator->Release(); enumerator = nullptr; }
        };

        HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator),
                                      nullptr,
                                      CLSCTX_ALL,
                                      __uuidof(IMMDeviceEnumerator),
                                      reinterpret_cast<void**>(&enumerator));
        if (FAILED(hr) || ! enumerator) {
            Sleep(500);
            continue;
        }

        hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
        if (FAILED(hr) || ! device) {
            cleanup();
            Sleep(500);
            continue;
        }

        hr = device->Activate(__uuidof(IAudioClient),
                              CLSCTX_ALL,
                              nullptr,
                              reinterpret_cast<void**>(&client));
        if (FAILED(hr) || ! client) {
            cleanup();
            Sleep(500);
            continue;
        }

        hr = client->GetMixFormat(&wfex);
        if (FAILED(hr) || ! wfex || wfex->nChannels == 0) {
            cleanup();
            Sleep(500);
            continue;
        }

        const bool isFloat = IsFloatFormat(wfex);
        const bool isPcm16 = IsPcm16Format(wfex);
        if (! isFloat && ! isPcm16) {
            cleanup();
            Sleep(500);
            continue;
        }

        // 100 ms buffer duration in 100-ns units
        constexpr REFERENCE_TIME kBufferDuration = 1000000;
        hr = client->Initialize(
            AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, kBufferDuration, 0, wfex, nullptr);
        if (FAILED(hr)) {
            cleanup();
            Sleep(500);
            continue;
        }

        hr = client->GetService(__uuidof(IAudioCaptureClient),
                                reinterpret_cast<void**>(&capture));
        if (FAILED(hr) || ! capture) {
            cleanup();
            Sleep(500);
            continue;
        }

        hr = client->Start();
        if (FAILED(hr)) {
            LOG_ERROR("WasapiAudioAnalyzer: IAudioClient::Start failed hr=0x%08X", static_cast<unsigned>(hr));
            cleanup();
            Sleep(500);
            continue;
        }

        m_sampleRate            = wfex->nSamplesPerSec > 0 ? wfex->nSamplesPerSec : 48000;
        const uint32_t channels = wfex->nChannels;
        LOG_INFO("WasapiAudioAnalyzer: loopback active (%u Hz, %u ch, %s)",
                 m_sampleRate,
                 channels,
                 isFloat ? "float32" : "pcm16");

        int emptyPolls = 0;
        while (m_running.load()) {
            if (m_paused.load()) {
                Sleep(40);
                continue;
            }

            UINT32 packetLength = 0;
            hr                  = capture->GetNextPacketSize(&packetLength);
            if (FAILED(hr)) break;

            if (packetLength == 0) {
                ++emptyPolls;
                if (emptyPolls >= 3) {
                    // Smooth decay only after sustained silence (~45 ms without audio packets)
                    PushSamplesAndAnalyze(zeros.data(), zeros.data(), zeros.size());
                }
                Sleep(15);
                continue;
            }
            emptyPolls = 0;

            while (packetLength > 0 && m_running.load()) {
                BYTE*  data      = nullptr;
                UINT32 numFrames = 0;
                DWORD  flags     = 0;

                hr = capture->GetBuffer(&data, &numFrames, &flags, nullptr, nullptr);
                if (FAILED(hr)) break;

                if (numFrames > 0) {
                    tmpL.resize(numFrames);
                    tmpR.resize(numFrames);
                    if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0 || data == nullptr) {
                        std::fill(tmpL.begin(), tmpL.end(), 0.0f);
                        std::fill(tmpR.begin(), tmpR.end(), 0.0f);
                    } else if (isFloat) {
                        const auto* f = reinterpret_cast<const float*>(data);
                        for (UINT32 i = 0; i < numFrames; ++i) {
                            tmpL[i] = f[i * channels];
                            tmpR[i] = (channels > 1) ? f[i * channels + 1] : tmpL[i];
                        }
                    } else {
                        const auto* s = reinterpret_cast<const int16_t*>(data);
                        constexpr float kScale = 1.0f / 32768.0f;
                        for (UINT32 i = 0; i < numFrames; ++i) {
                            tmpL[i] = static_cast<float>(s[i * channels]) * kScale;
                            tmpR[i] = (channels > 1) ? static_cast<float>(s[i * channels + 1]) * kScale
                                                     : tmpL[i];
                        }
                    }
                    PushSamplesAndAnalyze(tmpL.data(), tmpR.data(), numFrames);
                }

                capture->ReleaseBuffer(numFrames);
                hr = capture->GetNextPacketSize(&packetLength);
                if (FAILED(hr)) break;
            }
            if (FAILED(hr)) break;
            Sleep(10);
        }

        cleanup();
        if (m_running.load()) Sleep(300);
    }

    if (comOk) CoUninitialize();
}

} // namespace lcdfusion_wpe

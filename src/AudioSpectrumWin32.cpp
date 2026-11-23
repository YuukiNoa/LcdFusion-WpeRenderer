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
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdint>
#include <vector>

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#    define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

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

struct FftTables2048 {
    static constexpr std::size_t N = 2048;
    std::array<std::size_t, N>   bitRev {};
    // Precomputed twiddle factors for stages len = 2, 4, ..., N (total N - 1 entries)
    std::array<std::complex<float>, N> twiddles {};

    FftTables2048() {
        for (std::size_t i = 0; i < N; ++i) {
            std::size_t x = i;
            std::size_t r = 0;
            for (int b = 0; b < 11; ++b) {
                r = (r << 1) | (x & 1u);
                x >>= 1;
            }
            bitRev[i] = r;
        }
        std::size_t offset = 0;
        for (std::size_t len = 2; len <= N; len <<= 1) {
            std::size_t half  = len >> 1;
            double      angle = -2.0 * kPi / static_cast<double>(len);
            for (std::size_t j = 0; j < half; ++j) {
                double a           = angle * static_cast<double>(j);
                twiddles[offset + j] = std::complex<float>(static_cast<float>(std::cos(a)),
                                                           static_cast<float>(std::sin(a)));
            }
            offset += half;
        }
    }
};

const FftTables2048& GetFftTables() {
    static const FftTables2048 kTables;
    return kTables;
}

void Radix2Fft2048(std::array<std::complex<float>, 2048>& a) {
    constexpr std::size_t N      = 2048;
    const auto&           tables = GetFftTables();
    for (std::size_t i = 0; i < N; ++i) {
        std::size_t j = tables.bitRev[i];
        if (i < j) std::swap(a[i], a[j]);
    }
    std::size_t offset = 0;
    for (std::size_t len = 2; len <= N; len <<= 1) {
        std::size_t half = len >> 1;
        for (std::size_t i = 0; i < N; i += len) {
            for (std::size_t j = 0; j < half; ++j) {
                auto u             = a[i + j];
                auto v             = a[i + j + half] * tables.twiddles[offset + j];
                a[i + j]           = u + v;
                a[i + j + half]    = u - v;
            }
        }
        offset += half;
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

void WaitHighResMs(HANDLE hTimer, double ms) {
    if (hTimer != nullptr) {
        LARGE_INTEGER due {};
        due.QuadPart = -static_cast<LONGLONG>(ms * 10000.0);
        if (SetWaitableTimer(hTimer, &due, 0, nullptr, nullptr, FALSE)) {
            WaitForSingleObject(hTimer, static_cast<DWORD>(ms + 20.0));
            return;
        }
    }
    Sleep(std::max<DWORD>(1, static_cast<DWORD>(ms)));
}

} // namespace

WasapiAudioAnalyzer::WasapiAudioAnalyzer() {
    // Smooth asymmetric Hann window peaking at u = 0.72 (toward newest samples):
    // reduces window group delay from ~10.7 ms to ~6.0 ms at 48 kHz while preserving clean sidelobes
    // and coherent gain of ~0.5.
    constexpr double kPeak = 0.72;
    for (std::size_t i = 0; i < kWindowSize; ++i) {
        double u    = static_cast<double>(i) / static_cast<double>(kWindowSize - 1);
        double s    = (u < kPeak) ? (0.5 * u / kPeak) : (0.5 + 0.5 * (u - kPeak) / (1.0 - kPeak));
        m_window[i] = static_cast<float>(0.5 * (1.0 - std::cos(2.0 * kPi * s)));
    }
}

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
    PushSamplesOnly(left, right, frameCount);
    float dt = static_cast<float>(frameCount) / static_cast<float>(m_sampleRate > 0 ? m_sampleRate : 48000);
    ComputeBandsFromWindow(std::clamp(dt, 0.005f, 0.05f));
}

void WasapiAudioAnalyzer::PushSamplesOnly(const float* left, const float* right,
                                          std::size_t frameCount) {
    for (std::size_t i = 0; i < frameCount; ++i) {
        m_ringL[m_ringPos] = left[i];
        m_ringR[m_ringPos] = right[i];
        m_ringPos          = (m_ringPos + 1) % kWindowSize;
    }
}

void WasapiAudioAnalyzer::DecayTowardSilence(float dtSec) {
    constexpr float kTauRelease = 0.055f;
    float           decay       = std::exp(-std::clamp(dtSec, 0.001f, 0.1f) / kTauRelease);

    std::lock_guard<std::mutex> lock(m_mutex);
    for (std::size_t b = 0; b < 64; ++b) {
        m_spectrum.left64[b]  = (m_spectrum.left64[b] < 0.001f) ? 0.0f : (m_spectrum.left64[b] * decay);
        m_spectrum.right64[b] = (m_spectrum.right64[b] < 0.001f) ? 0.0f : (m_spectrum.right64[b] * decay);
    }
    for (std::size_t b = 0; b < 32; ++b) {
        float l0              = m_spectrum.left64[b * 2];
        float l1              = m_spectrum.left64[b * 2 + 1];
        float r0              = m_spectrum.right64[b * 2];
        float r1              = m_spectrum.right64[b * 2 + 1];
        m_spectrum.left32[b]  = 0.65f * (0.5f * (l0 + l1)) + 0.35f * std::max(l0, l1);
        m_spectrum.right32[b] = 0.65f * (0.5f * (r0 + r1)) + 0.35f * std::max(r0, r1);
    }
    for (std::size_t b = 0; b < 16; ++b) {
        float l0              = m_spectrum.left32[b * 2];
        float l1              = m_spectrum.left32[b * 2 + 1];
        float r0              = m_spectrum.right32[b * 2];
        float r1              = m_spectrum.right32[b * 2 + 1];
        m_spectrum.left16[b]  = 0.65f * (0.5f * (l0 + l1)) + 0.35f * std::max(l0, l1);
        m_spectrum.right16[b] = 0.65f * (0.5f * (r0 + r1)) + 0.35f * std::max(r0, r1);
    }
}

void WasapiAudioAnalyzer::ComputeBandsFromWindow(float dtSec) {
    std::array<std::complex<float>, kFftSize> fftL {};
    std::array<std::complex<float>, kFftSize> fftR {};

    // Remove DC offset so low bands [0..2] never get biased by DC drift.
    float meanL = 0.0f;
    float meanR = 0.0f;
    for (std::size_t i = 0; i < kWindowSize; ++i) {
        meanL += m_ringL[i];
        meanR += m_ringR[i];
    }
    meanL /= static_cast<float>(kWindowSize);
    meanR /= static_cast<float>(kWindowSize);

    for (std::size_t i = 0; i < kWindowSize; ++i) {
        std::size_t idx = (m_ringPos + i) % kWindowSize;
        float       w   = m_window[i];
        fftL[i]         = std::complex<float>((m_ringL[idx] - meanL) * w, 0.0f);
        fftR[i]         = std::complex<float>((m_ringR[idx] - meanR) * w, 0.0f);
    }
    // Remaining samples [kWindowSize .. kFftSize - 1] stay 0 (2x zero-padding for 23.4 Hz bin spacing).

    Radix2Fft2048(fftL);
    Radix2Fft2048(fftR);

    // Precompute normalized single-sided magnitude spectrum [0 .. kFftSize/2].
    // Coherent gain of asymmetric Hann window over kWindowSize samples is 0.5, single-sided factor = 2,
    // so full-scale sine has magnitude = kWindowSize * 0.25.
    constexpr std::size_t           kHalfBins = kFftSize / 2;
    constexpr float                 kNorm     = 1.0f / (static_cast<float>(kWindowSize) * 0.25f);
    std::array<float, kHalfBins + 1> magSpecL {};
    std::array<float, kHalfBins + 1> magSpecR {};
    for (std::size_t k = 1; k <= kHalfBins; ++k) {
        magSpecL[k] = std::abs(fftL[k]) * kNorm;
        magSpecR[k] = std::abs(fftR[k]) * kNorm;
    }

    // Wallpaper Engine compatible 64-band perceptual curve:
    // 30 Hz .. 12000 Hz with power exponent 1.85 gives:
    //   - bands 0..10  -> 30..365 Hz (sub-bass + kick + bass)
    //   - bands 11..32 -> 365..2640 Hz (low-mid + vocals)
    //   - bands 33..63 -> 2640..12000 Hz (presence + cymbals/air)
    // Fractional sub-bin interpolation eliminates low-band staircase duplicates.
    const double sr       = static_cast<double>(m_sampleRate > 0 ? m_sampleRate : 48000);
    const double binHz    = sr / static_cast<double>(kFftSize);
    const double fMin     = 30.0;
    const double fMax     = std::min(12000.0, sr * 0.45);
    constexpr double kExp = 1.85;

    auto sampleFracBin = [](const std::array<float, kHalfBins + 1>& spec, double binF) -> float {
        double clamped = std::clamp(binF, 1.0, static_cast<double>(kHalfBins - 1));
        auto   i0      = static_cast<std::size_t>(std::floor(clamped));
        auto   i1      = (i0 + 1 <= kHalfBins) ? (i0 + 1) : kHalfBins;
        float  frac    = static_cast<float>(clamped - static_cast<double>(i0));
        return spec[i0] * (1.0f - frac) + spec[i1] * frac;
    };

    auto shapeMagnitude = [](float mag, double fCenter) -> float {
        // Pink-noise spectral tilt compensation (+~2.7 dB/octave above 220 Hz) matching musical energy.
        float tilt = static_cast<float>(std::pow(std::max(fCenter, 30.0) / 220.0, 0.45));
        tilt       = std::clamp(tilt, 0.72f, 4.2f);
        // Scale linear amplitude to Wallpaper Engine's expected ~0..1.2 range with clean -52 dB noise gate.
        float v = mag * tilt * 2.2f;
        if (v < 0.005f) return 0.0f;
        v -= 0.005f;
        // Linear below 1.0 (preserves sharp dynamic punch), gentle soft-knee above 1.0.
        if (v > 1.0f) {
            v = 1.0f + (v - 1.0f) / (1.0f + 0.55f * (v - 1.0f));
        }
        return std::clamp(v, 0.0f, 1.6f);
    };

    std::array<float, 64> rawL {};
    std::array<float, 64> rawR {};

    for (std::size_t b = 0; b < 64; ++b) {
        double t0 = static_cast<double>(b) / 64.0;
        double t1 = static_cast<double>(b + 1) / 64.0;
        double f0 = fMin + (fMax - fMin) * std::pow(t0, kExp);
        double f1 = fMin + (fMax - fMin) * std::pow(t1, kExp);
        double fc = 0.5 * (f0 + f1);

        double b0 = f0 / binHz;
        double b1 = f1 / binHz;

        float valL = 0.0f;
        float valR = 0.0f;
        if (b1 - b0 <= 1.25) {
            // Narrow low-frequency band: smooth 3-tap fractional interpolation (no duplicate staircase).
            double bm = 0.5 * (b0 + b1);
            valL = 0.25f * sampleFracBin(magSpecL, b0) + 0.50f * sampleFracBin(magSpecL, bm) +
                   0.25f * sampleFracBin(magSpecL, b1);
            valR = 0.25f * sampleFracBin(magSpecR, b0) + 0.50f * sampleFracBin(magSpecR, bm) +
                   0.25f * sampleFracBin(magSpecR, b1);
        } else {
            // Wider mid/high band: RMS + peak blend so narrow tonal harmonics and broadband noise both track well.
            auto k0 = std::clamp<std::size_t>(static_cast<std::size_t>(std::floor(b0)), 1, kHalfBins);
            auto k1 = std::clamp<std::size_t>(static_cast<std::size_t>(std::ceil(b1)), k0, kHalfBins);
            float sumSqL = 0.0f, sumSqR = 0.0f;
            float peakL = 0.0f, peakR = 0.0f;
            std::size_t cnt = 0;
            for (std::size_t k = k0; k <= k1; ++k) {
                float mL = magSpecL[k];
                float mR = magSpecR[k];
                sumSqL += mL * mL;
                sumSqR += mR * mR;
                peakL = std::max(peakL, mL);
                peakR = std::max(peakR, mR);
                ++cnt;
            }
            float rmsL = std::sqrt(sumSqL / static_cast<float>(std::max<std::size_t>(1, cnt)));
            float rmsR = std::sqrt(sumSqR / static_cast<float>(std::max<std::size_t>(1, cnt)));
            valL       = 0.65f * rmsL + 0.35f * peakL;
            valR       = 0.65f * rmsR + 0.35f * peakR;
        }

        rawL[b] = shapeMagnitude(valL, fc);
        rawR[b] = shapeMagnitude(valR, fc);
    }

    // Time-normalized fast-attack / crisp-release envelope:
    // Near-instant attack (tau = 4 ms -> >92% on first 10 ms packet) avoids double-lag with shader smoothing,
    // while tauRelease = 55 ms gives smooth, responsive decay matching Wallpaper Engine.
    const float dt           = std::clamp(dtSec, 0.001f, 0.1f);
    const float alphaAttack  = 1.0f - std::exp(-dt / 0.004f);
    const float alphaRelease = 1.0f - std::exp(-dt / 0.055f);

    std::lock_guard<std::mutex> lock(m_mutex);
    for (std::size_t b = 0; b < 64; ++b) {
        float aL              = (rawL[b] >= m_spectrum.left64[b]) ? alphaAttack : alphaRelease;
        float aR              = (rawR[b] >= m_spectrum.right64[b]) ? alphaAttack : alphaRelease;
        m_spectrum.left64[b]  = m_spectrum.left64[b] + (rawL[b] - m_spectrum.left64[b]) * aL;
        m_spectrum.right64[b] = m_spectrum.right64[b] + (rawR[b] - m_spectrum.right64[b]) * aR;
    }
    for (std::size_t b = 0; b < 32; ++b) {
        float l0              = m_spectrum.left64[b * 2];
        float l1              = m_spectrum.left64[b * 2 + 1];
        float r0              = m_spectrum.right64[b * 2];
        float r1              = m_spectrum.right64[b * 2 + 1];
        m_spectrum.left32[b]  = 0.65f * (0.5f * (l0 + l1)) + 0.35f * std::max(l0, l1);
        m_spectrum.right32[b] = 0.65f * (0.5f * (r0 + r1)) + 0.35f * std::max(r0, r1);
    }
    for (std::size_t b = 0; b < 16; ++b) {
        float l0              = m_spectrum.left32[b * 2];
        float l1              = m_spectrum.left32[b * 2 + 1];
        float r0              = m_spectrum.right32[b * 2];
        float r1              = m_spectrum.right32[b * 2 + 1];
        m_spectrum.left16[b]  = 0.65f * (0.5f * (l0 + l1)) + 0.35f * std::max(l0, l1);
        m_spectrum.right16[b] = 0.65f * (0.5f * (r0 + r1)) + 0.35f * std::max(r0, r1);
    }
}

void WasapiAudioAnalyzer::CaptureLoop() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);

    HRESULT hrCom = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    bool    comOk = SUCCEEDED(hrCom);

    HANDLE hTimer = CreateWaitableTimerExW(
        nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (! hTimer) {
        hTimer = CreateWaitableTimerW(nullptr, FALSE, nullptr);
    }

    std::vector<float> tmpL;
    std::vector<float> tmpR;

    while (m_running.load()) {
        if (m_paused.load()) {
            WaitHighResMs(hTimer, 30.0);
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
            WaitHighResMs(hTimer, 400.0);
            continue;
        }

        hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
        if (FAILED(hr) || ! device) {
            cleanup();
            WaitHighResMs(hTimer, 400.0);
            continue;
        }

        hr = device->Activate(__uuidof(IAudioClient),
                              CLSCTX_ALL,
                              nullptr,
                              reinterpret_cast<void**>(&client));
        if (FAILED(hr) || ! client) {
            cleanup();
            WaitHighResMs(hTimer, 400.0);
            continue;
        }

        hr = client->GetMixFormat(&wfex);
        if (FAILED(hr) || ! wfex || wfex->nChannels == 0) {
            cleanup();
            WaitHighResMs(hTimer, 400.0);
            continue;
        }

        const bool isFloat = IsFloatFormat(wfex);
        const bool isPcm16 = IsPcm16Format(wfex);
        if (! isFloat && ! isPcm16) {
            cleanup();
            WaitHighResMs(hTimer, 400.0);
            continue;
        }

        REFERENCE_TIME defPeriod = 100000; // 10 ms default
        REFERENCE_TIME minPeriod = 30000;
        (void)client->GetDevicePeriod(&defPeriod, &minPeriod);
        // Keep shared loopback buffer tight (~20 ms) for low latency while avoiding underruns.
        REFERENCE_TIME bufDuration = std::clamp<REFERENCE_TIME>(defPeriod * 2, 200000, 400000);

        hr = client->Initialize(
            AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, bufDuration, 0, wfex, nullptr);
        if (FAILED(hr)) {
            cleanup();
            WaitHighResMs(hTimer, 400.0);
            continue;
        }

        hr = client->GetService(__uuidof(IAudioCaptureClient),
                                reinterpret_cast<void**>(&capture));
        if (FAILED(hr) || ! capture) {
            cleanup();
            WaitHighResMs(hTimer, 400.0);
            continue;
        }

        hr = client->Start();
        if (FAILED(hr)) {
            LOG_ERROR("WasapiAudioAnalyzer: IAudioClient::Start failed hr=0x%08X", static_cast<unsigned>(hr));
            cleanup();
            WaitHighResMs(hTimer, 400.0);
            continue;
        }

        m_sampleRate            = wfex->nSamplesPerSec > 0 ? wfex->nSamplesPerSec : 48000;
        const uint32_t channels = wfex->nChannels;
        LOG_INFO("WasapiAudioAnalyzer: loopback active (%u Hz, %u ch, %s, buf=%.1fms)",
                 m_sampleRate,
                 channels,
                 isFloat ? "float32" : "pcm16",
                 static_cast<double>(bufDuration) / 10000.0);

        auto lastAnalyzeTime = std::chrono::steady_clock::now();
        auto lastPacketTime  = lastAnalyzeTime;

        while (m_running.load()) {
            if (m_paused.load()) {
                WaitHighResMs(hTimer, 30.0);
                lastAnalyzeTime = std::chrono::steady_clock::now();
                lastPacketTime  = lastAnalyzeTime;
                continue;
            }

            UINT32 packetLength = 0;
            hr                  = capture->GetNextPacketSize(&packetLength);
            if (FAILED(hr)) break;

            auto now = std::chrono::steady_clock::now();

            if (packetLength == 0) {
                // Only decay toward silence if WASAPI loopback has produced no packets for > 35 ms.
                double silentMs = std::chrono::duration<double, std::milli>(now - lastPacketTime).count();
                if (silentMs >= 35.0) {
                    float dt        = std::chrono::duration<float>(now - lastAnalyzeTime).count();
                    lastAnalyzeTime = now;
                    DecayTowardSilence(dt);
                }
                WaitHighResMs(hTimer, 2.5);
                continue;
            }

            // Drain all queued packets into the ring buffer first, then analyze the freshest window once.
            bool anySamples = false;
            bool allSilent  = true;
            while (packetLength > 0 && m_running.load()) {
                BYTE*  data      = nullptr;
                UINT32 numFrames = 0;
                DWORD  flags     = 0;

                hr = capture->GetBuffer(&data, &numFrames, &flags, nullptr, nullptr);
                if (FAILED(hr)) break;

                if (numFrames > 0) {
                    anySamples = true;
                    tmpL.resize(numFrames);
                    tmpR.resize(numFrames);
                    if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0 || data == nullptr) {
                        std::fill(tmpL.begin(), tmpL.end(), 0.0f);
                        std::fill(tmpR.begin(), tmpR.end(), 0.0f);
                    } else {
                        allSilent = false;
                        if (isFloat) {
                            const auto* f = reinterpret_cast<const float*>(data);
                            for (UINT32 i = 0; i < numFrames; ++i) {
                                tmpL[i] = f[i * channels];
                                tmpR[i] = (channels > 1) ? f[i * channels + 1] : tmpL[i];
                            }
                        } else {
                            const auto* s          = reinterpret_cast<const int16_t*>(data);
                            constexpr float kScale = 1.0f / 32768.0f;
                            for (UINT32 i = 0; i < numFrames; ++i) {
                                tmpL[i] = static_cast<float>(s[i * channels]) * kScale;
                                tmpR[i] = (channels > 1) ? static_cast<float>(s[i * channels + 1]) * kScale
                                                         : tmpL[i];
                            }
                        }
                    }
                    PushSamplesOnly(tmpL.data(), tmpR.data(), numFrames);
                }

                capture->ReleaseBuffer(numFrames);
                hr = capture->GetNextPacketSize(&packetLength);
                if (FAILED(hr)) break;
            }
            if (FAILED(hr)) break;

            if (anySamples) {
                now             = std::chrono::steady_clock::now();
                float dt        = std::chrono::duration<float>(now - lastAnalyzeTime).count();
                lastAnalyzeTime = now;
                lastPacketTime  = now;
                if (allSilent) {
                    DecayTowardSilence(dt);
                } else {
                    ComputeBandsFromWindow(dt);
                }
            }

            WaitHighResMs(hTimer, 2.5);
        }

        cleanup();
        if (m_running.load()) WaitHighResMs(hTimer, 250.0);
    }

    if (hTimer) CloseHandle(hTimer);
    if (comOk) CoUninitialize();
}

} // namespace lcdfusion_wpe

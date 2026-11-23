#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <thread>

namespace lcdfusion_wpe
{

struct AudioSpectrumData {
    std::array<float, 64> left64 {};
    std::array<float, 64> right64 {};
    std::array<float, 32> left32 {};
    std::array<float, 32> right32 {};
    std::array<float, 16> left16 {};
    std::array<float, 16> right16 {};
};

// Captures system audio via Windows WASAPI Loopback and computes smoothed 64/32/16-band stereo FFT spectrum.
class WasapiAudioAnalyzer {
public:
    WasapiAudioAnalyzer();
    ~WasapiAudioAnalyzer();

    WasapiAudioAnalyzer(const WasapiAudioAnalyzer&)            = delete;
    WasapiAudioAnalyzer& operator=(const WasapiAudioAnalyzer&) = delete;

    void Start();
    void Stop();
    void SetPaused(bool paused);

    void GetSpectrum(AudioSpectrumData& out) const;

    // Deterministic PCM injection for offline testing / verification.
    void FeedPcmSamples(const float* left, const float* right, std::size_t frameCount);

private:
    void CaptureLoop();
    void PushSamplesOnly(const float* left, const float* right, std::size_t frameCount);
    void ComputeBandsFromWindow(float dtSec);
    void DecayTowardSilence(float dtSec);

    std::atomic<bool> m_running { false };
    std::atomic<bool> m_paused { false };
    std::thread       m_thread;

    mutable std::mutex m_mutex;
    AudioSpectrumData  m_spectrum {};

    // 1024-sample (~21.3 ms @ 48 kHz) capture window zero-padded to 2048-point FFT (23.4 Hz spacing)
    // for low latency + fine low-frequency resolution without staircase artifacts.
    static constexpr std::size_t kWindowSize = 1024;
    static constexpr std::size_t kFftSize    = 2048;
    std::array<float, kWindowSize> m_ringL {};
    std::array<float, kWindowSize> m_ringR {};
    std::array<float, kWindowSize> m_window {};
    std::size_t                    m_ringPos { 0 };
    uint32_t                       m_sampleRate { 48000 };
};

} // namespace lcdfusion_wpe

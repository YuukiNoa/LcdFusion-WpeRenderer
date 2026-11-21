#include "Audio/SoundManager.h"

namespace wallpaper
{
namespace audio
{

namespace
{
class NullSoundStream final : public SoundStream {
public:
    uint64_t NextPcmData(void*, uint32_t) override { return 0; }
    void     PassDesc(const Desc&) override {}
};
} // namespace

std::unique_ptr<SoundStream> CreateSoundStream(std::shared_ptr<fs::IBinaryStream>,
                                               const SoundStream::Desc&) {
    return std::make_unique<NullSoundStream>();
}

class SoundManager::impl {
public:
    bool  inited { false };
    bool  muted { true };
    float volume { 0.0f };
};

SoundManager::SoundManager(): pImpl(std::make_unique<impl>()) {}
SoundManager::~SoundManager() = default;

void  SoundManager::MountStream(std::unique_ptr<SoundStream>&&) {}
void  SoundManager::UnMountAll() {}
void  SoundManager::Test(std::shared_ptr<fs::IBinaryStream>) {}
bool  SoundManager::Init() { pImpl->inited = true; return true; }
bool  SoundManager::IsInited() const { return pImpl->inited; }
void  SoundManager::Play() {}
void  SoundManager::Pause() {}
float SoundManager::Volume() const { return pImpl->volume; }
bool  SoundManager::Muted() const { return pImpl->muted; }
void  SoundManager::SetMuted(bool v) { pImpl->muted = v; }
void  SoundManager::SetVolume(float v) { pImpl->volume = v; }

} // namespace audio
} // namespace wallpaper

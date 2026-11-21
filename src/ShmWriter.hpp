#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace lcdfusion_wpe
{

constexpr uint32_t kShmMagic   = 0x4C434457u; // "LCDW"
constexpr uint32_t kShmVersion = 1u;

constexpr uint32_t kShmStateInit    = 0u;
constexpr uint32_t kShmStateRunning = 1u;
constexpr uint32_t kShmStatePaused  = 2u;
constexpr uint32_t kShmStateError   = 3u;

#pragma pack(push, 1)
struct ShmHeader {
    uint32_t magic;       // offset 0
    uint32_t version;     // offset 4
    uint32_t width;       // offset 8
    uint32_t height;      // offset 12
    uint32_t stride;      // offset 16
    uint32_t activeSlot;  // offset 20
    uint64_t frameSeq;    // offset 24
    uint32_t state;       // offset 32
    uint32_t reserved[7]; // offset 36..63
};
#pragma pack(pop)

static_assert(sizeof(ShmHeader) == 64, "ShmHeader must be exactly 64 bytes");
static_assert(offsetof(ShmHeader, magic) == 0);
static_assert(offsetof(ShmHeader, version) == 4);
static_assert(offsetof(ShmHeader, width) == 8);
static_assert(offsetof(ShmHeader, height) == 12);
static_assert(offsetof(ShmHeader, stride) == 16);
static_assert(offsetof(ShmHeader, activeSlot) == 20);
static_assert(offsetof(ShmHeader, frameSeq) == 24);
static_assert(offsetof(ShmHeader, state) == 32);

class ShmWriter {
public:
    ShmWriter() = default;
    ~ShmWriter();

    ShmWriter(const ShmWriter&)            = delete;
    ShmWriter& operator=(const ShmWriter&) = delete;

    bool Open(const std::string& shmNameUtf8, uint32_t width, uint32_t height, std::string& outError);
    void Close();
    bool IsOpen() const { return m_view != nullptr; }

    void SetState(uint32_t state);
    bool PublishFrame(const uint8_t* bgraPixels, uint32_t width, uint32_t height, uint32_t srcStride);

    uint64_t FrameSeq() const { return m_frameSeq; }

private:
    void*       m_mapping { nullptr };
    uint8_t*    m_view { nullptr };
    std::size_t m_totalBytes { 0 };
    uint32_t    m_width { 0 };
    uint32_t    m_height { 0 };
    uint32_t    m_stride { 0 };
    uint32_t    m_activeSlot { 0 };
    uint64_t    m_frameSeq { 0 };
};

} // namespace lcdfusion_wpe

#include "ShmWriter.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <atomic>
#include <cstring>

namespace lcdfusion_wpe
{

namespace
{
std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    int needed = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (needed <= 0) return {};
    std::wstring out(static_cast<std::size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), needed);
    return out;
}
} // namespace

ShmWriter::~ShmWriter() { Close(); }

bool ShmWriter::Open(const std::string& shmNameUtf8, uint32_t width, uint32_t height,
                     std::string& outError) {
    Close();

    if (shmNameUtf8.empty() || width == 0 || height == 0) {
        outError = "Invalid shared memory arguments";
        return false;
    }

    const std::size_t stride     = static_cast<std::size_t>(width) * 4u;
    const std::size_t slotBytes  = stride * static_cast<std::size_t>(height);
    const std::size_t totalBytes = sizeof(ShmHeader) + slotBytes * 2u;

    std::wstring wideName = Utf8ToWide(shmNameUtf8);
    HANDLE       hMap     = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, wideName.c_str());
    if (hMap == nullptr) {
        outError = "OpenFileMappingW failed (err=" + std::to_string(GetLastError()) + "): " + shmNameUtf8;
        return false;
    }

    void* view = MapViewOfFile(hMap, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, totalBytes);
    if (view == nullptr) {
        DWORD err = GetLastError();
        CloseHandle(hMap);
        outError = "MapViewOfFile failed (err=" + std::to_string(err) + ")";
        return false;
    }

    m_mapping    = hMap;
    m_view       = static_cast<uint8_t*>(view);
    m_totalBytes = totalBytes;
    m_width      = width;
    m_height     = height;
    m_stride     = static_cast<uint32_t>(stride);
    m_activeSlot = 0;
    m_frameSeq   = 0;

    auto* hdr        = reinterpret_cast<ShmHeader*>(m_view);
    hdr->magic       = kShmMagic;
    hdr->version     = kShmVersion;
    hdr->width       = m_width;
    hdr->height      = m_height;
    hdr->stride      = m_stride;
    hdr->activeSlot  = 0;
    hdr->frameSeq    = 0;
    hdr->state       = kShmStateRunning;
    std::atomic_thread_fence(std::memory_order_release);
    return true;
}

void ShmWriter::Close() {
    if (m_view != nullptr) {
        UnmapViewOfFile(m_view);
        m_view = nullptr;
    }
    if (m_mapping != nullptr) {
        CloseHandle(static_cast<HANDLE>(m_mapping));
        m_mapping = nullptr;
    }
    m_totalBytes = 0;
    m_width      = 0;
    m_height     = 0;
    m_stride     = 0;
    m_activeSlot = 0;
    m_frameSeq   = 0;
}

void ShmWriter::SetState(uint32_t state) {
    if (m_view == nullptr) return;
    auto* hdr = reinterpret_cast<ShmHeader*>(m_view);
    std::atomic_thread_fence(std::memory_order_release);
    hdr->state = state;
    std::atomic_thread_fence(std::memory_order_release);
}

bool ShmWriter::PublishFrame(const uint8_t* bgraPixels, uint32_t width, uint32_t height,
                             uint32_t srcStride) {
    if (m_view == nullptr || bgraPixels == nullptr) return false;
    if (width != m_width || height != m_height) return false;

    const uint32_t    nextSlot  = 1u - (m_activeSlot & 1u);
    const std::size_t slotBytes = static_cast<std::size_t>(m_stride) * m_height;
    uint8_t*          dst       = m_view + sizeof(ShmHeader) + static_cast<std::size_t>(nextSlot) * slotBytes;

    if (srcStride == m_stride) {
        std::memcpy(dst, bgraPixels, slotBytes);
    } else {
        for (uint32_t y = 0; y < m_height; ++y) {
            std::memcpy(dst + static_cast<std::size_t>(y) * m_stride,
                        bgraPixels + static_cast<std::size_t>(y) * srcStride,
                        m_stride);
        }
    }

    auto* hdr = reinterpret_cast<ShmHeader*>(m_view);
    std::atomic_thread_fence(std::memory_order_release);
    m_activeSlot    = nextSlot;
    hdr->activeSlot = nextSlot;
    std::atomic_thread_fence(std::memory_order_release);
    ++m_frameSeq;
    hdr->frameSeq = m_frameSeq;
    std::atomic_thread_fence(std::memory_order_release);
    return true;
}

} // namespace lcdfusion_wpe

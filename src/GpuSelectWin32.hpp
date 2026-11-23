#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace lcdfusion_wpe
{

struct GpuPreference {
    bool        hasLuid { false };
    int32_t     luidHigh { 0 };
    uint32_t    luidLow { 0 };
    uint32_t    vendorId { 0 };
    uint32_t    deviceId { 0 };
    int32_t     ordinal { -1 }; // 0-based index among identical vendorId:deviceId, or -1 if unspecified
    std::string name;
    std::string uuidHex;

    bool IsAuto() const {
        return ! hasLuid && vendorId == 0 && deviceId == 0 && name.empty() && uuidHex.empty();
    }
};

struct GpuDeviceInfo {
    std::string             name;
    std::string             type { "other" }; // discrete | integrated | virtual | cpu | other
    uint32_t                vendorId { 0 };
    uint32_t                deviceId { 0 };
    int32_t                 ordinal { 0 };
    bool                    luidValid { false };
    int32_t                 luidHigh { 0 };
    uint32_t                luidLow { 0 };
    std::array<uint8_t, 16> uuid {};
    std::string             uuidHex;
    uint64_t                vramBytes { 0 };
    bool                    vulkanCompatible { false };
};

// Parses CLI --gpu string:
//   "auto" / ""                   -> auto
//   "pci:VVVV:DDDD[:ORDINAL]"     -> vendorId + deviceId (+ optional ordinal)
//   "luid:HIGH:LOW"               -> exact LUID (decimal high,low)
//   32-char hex string            -> Vulkan deviceUUID
//   any other string              -> case-insensitive GPU name substring
GpuPreference ParseGpuPreferenceString(const std::string& spec);

// Enumerates Vulkan physical devices visible on this machine (with UUID, LUID, VRAM, compatibility).
std::vector<GpuDeviceInfo> EnumerateVulkanGpus();

// Resolves a GpuPreference against compatible Vulkan devices.
// Returns true if a specific compatible Vulkan GPU was selected (or first compatible if pref.IsAuto()).
bool ResolvePreferredVulkanGpu(const GpuPreference& pref, GpuDeviceInfo& outGpu);

// Resolves a GpuPreference against hardware DXGI adapters to get its runtime Windows LUID.
// If pref.IsAuto(), returns false (caller should use system default GPU).
bool ResolvePreferredDxgiAdapterLuid(const GpuPreference& pref, int32_t& outHigh,
                                     uint32_t& outLow, std::string& outName);

} // namespace lcdfusion_wpe

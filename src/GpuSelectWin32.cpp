#include "GpuSelectWin32.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <dxgi.h>
#include <vulkan/vulkan.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <map>
#include <sstream>

namespace lcdfusion_wpe
{

namespace
{

std::string ToLowerAscii(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string TrimAscii(const std::string& s) {
    std::size_t a = 0;
    while (a < s.size() && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    std::size_t b = s.size();
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

std::string BytesToHexLower(const uint8_t* bytes, std::size_t len) {
    static const char kHex[] = "0123456789abcdef";
    std::string       out;
    out.reserve(len * 2);
    for (std::size_t i = 0; i < len; ++i) {
        out.push_back(kHex[(bytes[i] >> 4) & 0x0F]);
        out.push_back(kHex[bytes[i] & 0x0F]);
    }
    return out;
}

bool IsHex32(const std::string& s) {
    if (s.size() != 32) return false;
    for (char c : s) {
        if (! std::isxdigit(static_cast<unsigned char>(c))) return false;
    }
    return true;
}

std::string DeviceTypeToString(VkPhysicalDeviceType t) {
    switch (t) {
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: return "discrete";
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return "integrated";
    case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: return "virtual";
    case VK_PHYSICAL_DEVICE_TYPE_CPU: return "cpu";
    default: return "other";
    }
}

std::string WideToUtf8Local(const wchar_t* w) {
    if (! w || ! *w) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string out(static_cast<std::size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, out.data(), n, nullptr, nullptr);
    return out;
}

} // namespace

GpuPreference ParseGpuPreferenceString(const std::string& rawSpec) {
    GpuPreference pref {};
    std::string   spec = TrimAscii(rawSpec);
    if (spec.empty()) return pref;
    std::string lower = ToLowerAscii(spec);
    if (lower == "auto" || lower == "default") return pref;

    if (lower.rfind("pci:", 0) == 0) {
        std::string rest = lower.substr(4);
        std::replace(rest.begin(), rest.end(), ':', ' ');
        std::istringstream iss(rest);
        std::string        vHex, dHex;
        int32_t            ord = -1;
        if (iss >> vHex >> dHex) {
            try {
                pref.vendorId = static_cast<uint32_t>(std::stoul(vHex, nullptr, 16));
                pref.deviceId = static_cast<uint32_t>(std::stoul(dHex, nullptr, 16));
                if (iss >> ord) pref.ordinal = ord;
                return pref;
            } catch (...) {}
        }
    }

    if (lower.rfind("luid:", 0) == 0) {
        std::string rest = lower.substr(5);
        std::replace(rest.begin(), rest.end(), ':', ' ');
        std::replace(rest.begin(), rest.end(), ',', ' ');
        std::istringstream iss(rest);
        long long          hi = 0;
        unsigned long long lo = 0;
        if (iss >> hi >> lo) {
            pref.hasLuid  = true;
            pref.luidHigh = static_cast<int32_t>(hi);
            pref.luidLow  = static_cast<uint32_t>(lo);
            return pref;
        }
    }

    if (IsHex32(lower)) {
        pref.uuidHex = lower;
        return pref;
    }

    pref.name = spec;
    return pref;
}

std::vector<GpuDeviceInfo> EnumerateVulkanGpus() {
    std::vector<GpuDeviceInfo> result;

    HMODULE hVk = LoadLibraryW(L"vulkan-1.dll");
    if (! hVk) return result;

    auto pfnGetInstanceProcAddr =
        reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(hVk, "vkGetInstanceProcAddr"));
    if (! pfnGetInstanceProcAddr) {
        FreeLibrary(hVk);
        return result;
    }

    auto pfnCreateInstance = reinterpret_cast<PFN_vkCreateInstance>(
        pfnGetInstanceProcAddr(nullptr, "vkCreateInstance"));
    if (! pfnCreateInstance) {
        FreeLibrary(hVk);
        return result;
    }

    VkApplicationInfo appInfo {};
    appInfo.sType              = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName   = "wpe-renderer-gpu-enum";
    appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.pEngineName        = "vulkan";
    appInfo.engineVersion      = VK_MAKE_VERSION(1, 0, 0);
    appInfo.apiVersion         = VK_API_VERSION_1_1;

    VkInstanceCreateInfo createInfo {};
    createInfo.sType            = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    createInfo.pApplicationInfo = &appInfo;

    VkInstance inst = VK_NULL_HANDLE;
    if (pfnCreateInstance(&createInfo, nullptr, &inst) != VK_SUCCESS || inst == VK_NULL_HANDLE) {
        FreeLibrary(hVk);
        return result;
    }

    auto pfnDestroyInstance = reinterpret_cast<PFN_vkDestroyInstance>(
        pfnGetInstanceProcAddr(inst, "vkDestroyInstance"));
    auto pfnEnumeratePhysicalDevices = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(
        pfnGetInstanceProcAddr(inst, "vkEnumeratePhysicalDevices"));
    auto pfnGetPhysicalDeviceProperties = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(
        pfnGetInstanceProcAddr(inst, "vkGetPhysicalDeviceProperties"));
    auto pfnGetPhysicalDeviceProperties2 = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(
        pfnGetInstanceProcAddr(inst, "vkGetPhysicalDeviceProperties2"));
    auto pfnGetPhysicalDeviceMemoryProperties =
        reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(
            pfnGetInstanceProcAddr(inst, "vkGetPhysicalDeviceMemoryProperties"));
    auto pfnGetPhysicalDeviceQueueFamilyProperties =
        reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(
            pfnGetInstanceProcAddr(inst, "vkGetPhysicalDeviceQueueFamilyProperties"));
    auto pfnEnumerateDeviceExtensionProperties =
        reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(
            pfnGetInstanceProcAddr(inst, "vkEnumerateDeviceExtensionProperties"));

    if (pfnEnumeratePhysicalDevices) {
        uint32_t count = 0;
        if (pfnEnumeratePhysicalDevices(inst, &count, nullptr) == VK_SUCCESS && count > 0) {
            std::vector<VkPhysicalDevice> devs(count);
            if (pfnEnumeratePhysicalDevices(inst, &count, devs.data()) == VK_SUCCESS) {
                std::map<std::pair<uint32_t, uint32_t>, int32_t> ordinalMap;

                for (VkPhysicalDevice dev : devs) {
                    GpuDeviceInfo info {};

                    VkPhysicalDeviceIDProperties idProps {};
                    idProps.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;

                    VkPhysicalDeviceProperties2 props2 {};
                    props2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
                    props2.pNext = &idProps;

                    if (pfnGetPhysicalDeviceProperties2) {
                        pfnGetPhysicalDeviceProperties2(dev, &props2);
                    } else if (pfnGetPhysicalDeviceProperties) {
                        pfnGetPhysicalDeviceProperties(dev, &props2.properties);
                    }

                    const auto& p = props2.properties;
                    info.name     = p.deviceName;
                    info.type     = DeviceTypeToString(p.deviceType);
                    info.vendorId = p.vendorID;
                    info.deviceId = p.deviceID;
                    info.ordinal  = ordinalMap[{ p.vendorID, p.deviceID }]++;

                    std::memcpy(info.uuid.data(), idProps.deviceUUID, info.uuid.size());
                    info.uuidHex = BytesToHexLower(info.uuid.data(), info.uuid.size());

                    if (idProps.deviceLUIDValid == VK_TRUE) {
                        LUID luid {};
                        static_assert(sizeof(LUID) == VK_LUID_SIZE, "Unexpected LUID size");
                        std::memcpy(&luid, idProps.deviceLUID, sizeof(LUID));
                        info.luidValid = true;
                        info.luidHigh  = static_cast<int32_t>(luid.HighPart);
                        info.luidLow   = static_cast<uint32_t>(luid.LowPart);
                    }

                    if (pfnGetPhysicalDeviceMemoryProperties) {
                        VkPhysicalDeviceMemoryProperties memProps {};
                        pfnGetPhysicalDeviceMemoryProperties(dev, &memProps);
                        uint64_t totalLocal = 0;
                        for (uint32_t i = 0; i < memProps.memoryHeapCount; ++i) {
                            if ((memProps.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0) {
                                totalLocal += memProps.memoryHeaps[i].size;
                            }
                        }
                        info.vramBytes = totalLocal;
                    }

                    bool hasGraphics = false;
                    if (pfnGetPhysicalDeviceQueueFamilyProperties) {
                        uint32_t qCount = 0;
                        pfnGetPhysicalDeviceQueueFamilyProperties(dev, &qCount, nullptr);
                        if (qCount > 0) {
                            std::vector<VkQueueFamilyProperties> qProps(qCount);
                            pfnGetPhysicalDeviceQueueFamilyProperties(dev, &qCount, qProps.data());
                            for (const auto& q : qProps) {
                                if ((q.queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0) {
                                    hasGraphics = true;
                                    break;
                                }
                            }
                        }
                    }

                    bool hasPushDesc = false;
                    if (pfnEnumerateDeviceExtensionProperties) {
                        uint32_t extCount = 0;
                        if (pfnEnumerateDeviceExtensionProperties(dev, nullptr, &extCount, nullptr) == VK_SUCCESS &&
                            extCount > 0) {
                            std::vector<VkExtensionProperties> exts(extCount);
                            if (pfnEnumerateDeviceExtensionProperties(dev, nullptr, &extCount, exts.data()) ==
                                VK_SUCCESS) {
                                for (const auto& e : exts) {
                                    if (std::strcmp(e.extensionName, VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME) == 0) {
                                        hasPushDesc = true;
                                        break;
                                    }
                                }
                            }
                        }
                    }

                    info.vulkanCompatible = hasGraphics && hasPushDesc;
                    result.push_back(std::move(info));
                }
            }
        }
    }

    if (pfnDestroyInstance) {
        pfnDestroyInstance(inst, nullptr);
    }
    FreeLibrary(hVk);
    return result;
}

bool ResolvePreferredVulkanGpu(const GpuPreference& pref, GpuDeviceInfo& outGpu) {
    auto gpus = EnumerateVulkanGpus();
    if (gpus.empty()) return false;

    std::vector<const GpuDeviceInfo*> compat;
    for (const auto& g : gpus) {
        if (g.vulkanCompatible) compat.push_back(&g);
    }
    if (compat.empty()) return false;

    if (! pref.IsAuto()) {
        // 1. Exact Vulkan UUID match
        if (! pref.uuidHex.empty()) {
            std::string target = ToLowerAscii(pref.uuidHex);
            for (const auto* g : compat) {
                if (g->uuidHex == target) {
                    outGpu = *g;
                    return true;
                }
            }
        }

        // 2. Exact Windows LUID match (matches DXGI adapter LUID 1:1 on Windows 10/11)
        if (pref.hasLuid) {
            for (const auto* g : compat) {
                if (g->luidValid && g->luidHigh == pref.luidHigh && g->luidLow == pref.luidLow) {
                    outGpu = *g;
                    return true;
                }
            }
        }

        // 3. PCI vendorId + deviceId (+ optional ordinal)
        if (pref.vendorId != 0 && pref.deviceId != 0) {
            const GpuDeviceInfo* firstMatch = nullptr;
            for (const auto* g : compat) {
                if (g->vendorId == pref.vendorId && g->deviceId == pref.deviceId) {
                    if (! firstMatch) firstMatch = g;
                    if (pref.ordinal < 0 || g->ordinal == pref.ordinal) {
                        outGpu = *g;
                        return true;
                    }
                }
            }
            if (firstMatch) {
                outGpu = *firstMatch;
                return true;
            }
        }

        // 4. Case-insensitive name match (exact first, then substring)
        if (! pref.name.empty()) {
            std::string needle = ToLowerAscii(pref.name);
            for (const auto* g : compat) {
                if (ToLowerAscii(g->name) == needle) {
                    outGpu = *g;
                    return true;
                }
            }
            for (const auto* g : compat) {
                if (ToLowerAscii(g->name).find(needle) != std::string::npos) {
                    outGpu = *g;
                    return true;
                }
            }
        }
    }

    // Fallback (or auto): pick the first compatible Vulkan GPU (matches upstream behavior)
    outGpu = *compat.front();
    return true;
}

bool ResolvePreferredDxgiAdapterLuid(const GpuPreference& pref, int32_t& outHigh,
                                     uint32_t& outLow, std::string& outName) {
    if (pref.IsAuto()) return false;

    HMODULE hDxgi = LoadLibraryW(L"dxgi.dll");
    if (! hDxgi) {
        if (pref.hasLuid) {
            outHigh = pref.luidHigh;
            outLow  = pref.luidLow;
            outName = pref.name;
            return true;
        }
        return false;
    }

    using PfnCreateDXGIFactory1 = HRESULT(WINAPI*)(REFIID, void**);
    auto pfnCreate =
        reinterpret_cast<PfnCreateDXGIFactory1>(GetProcAddress(hDxgi, "CreateDXGIFactory1"));
    if (! pfnCreate) {
        FreeLibrary(hDxgi);
        return false;
    }

    IDXGIFactory1* factory = nullptr;
    HRESULT        hr      = pfnCreate(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory));
    if (FAILED(hr) || ! factory) {
        FreeLibrary(hDxgi);
        return false;
    }

    struct DxgiEntry {
        std::string name;
        uint32_t    vendorId;
        uint32_t    deviceId;
        int32_t     ordinal;
        int32_t     luidHigh;
        uint32_t    luidLow;
    };

    std::vector<DxgiEntry>                           entries;
    std::map<std::pair<uint32_t, uint32_t>, int32_t> ordinalMap;

    for (UINT idx = 0;; ++idx) {
        IDXGIAdapter1* adapter = nullptr;
        if (factory->EnumAdapters1(idx, &adapter) == DXGI_ERROR_NOT_FOUND || ! adapter) break;
        DXGI_ADAPTER_DESC1 desc {};
        if (SUCCEEDED(adapter->GetDesc1(&desc))) {
            bool isSoftware = (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0 ||
                              (desc.VendorId == 0x1414 && desc.DeviceId == 0x008C);
            if (! isSoftware) {
                DxgiEntry e {};
                e.name     = WideToUtf8Local(desc.Description);
                e.vendorId = desc.VendorId;
                e.deviceId = desc.DeviceId;
                e.ordinal  = ordinalMap[{ desc.VendorId, desc.DeviceId }]++;
                e.luidHigh = static_cast<int32_t>(desc.AdapterLuid.HighPart);
                e.luidLow  = static_cast<uint32_t>(desc.AdapterLuid.LowPart);
                entries.push_back(std::move(e));
            }
        }
        adapter->Release();
    }
    factory->Release();
    FreeLibrary(hDxgi);

    if (entries.empty()) {
        if (pref.hasLuid) {
            outHigh = pref.luidHigh;
            outLow  = pref.luidLow;
            outName = pref.name;
            return true;
        }
        return false;
    }

    // 1. Exact LUID match
    if (pref.hasLuid) {
        for (const auto& e : entries) {
            if (e.luidHigh == pref.luidHigh && e.luidLow == pref.luidLow) {
                outHigh = e.luidHigh;
                outLow  = e.luidLow;
                outName = e.name;
                return true;
            }
        }
    }

    // 2. PCI vendorId + deviceId (+ optional ordinal)
    if (pref.vendorId != 0 && pref.deviceId != 0) {
        const DxgiEntry* firstMatch = nullptr;
        for (const auto& e : entries) {
            if (e.vendorId == pref.vendorId && e.deviceId == pref.deviceId) {
                if (! firstMatch) firstMatch = &e;
                if (pref.ordinal < 0 || e.ordinal == pref.ordinal) {
                    outHigh = e.luidHigh;
                    outLow  = e.luidLow;
                    outName = e.name;
                    return true;
                }
            }
        }
        if (firstMatch) {
            outHigh = firstMatch->luidHigh;
            outLow  = firstMatch->luidLow;
            outName = firstMatch->name;
            return true;
        }
    }

    // 3. Name match
    if (! pref.name.empty()) {
        std::string needle = ToLowerAscii(pref.name);
        for (const auto& e : entries) {
            if (ToLowerAscii(e.name) == needle) {
                outHigh = e.luidHigh;
                outLow  = e.luidLow;
                outName = e.name;
                return true;
            }
        }
        for (const auto& e : entries) {
            if (ToLowerAscii(e.name).find(needle) != std::string::npos) {
                outHigh = e.luidHigh;
                outLow  = e.luidLow;
                outName = e.name;
                return true;
            }
        }
    }

    // 4. If Vulkan UUID was specified, map via Vulkan enumeration -> LUID
    if (! pref.uuidHex.empty()) {
        GpuDeviceInfo vkGpu {};
        if (ResolvePreferredVulkanGpu(pref, vkGpu) && vkGpu.luidValid) {
            outHigh = vkGpu.luidHigh;
            outLow  = vkGpu.luidLow;
            outName = vkGpu.name;
            return true;
        }
    }

    return false;
}

} // namespace lcdfusion_wpe

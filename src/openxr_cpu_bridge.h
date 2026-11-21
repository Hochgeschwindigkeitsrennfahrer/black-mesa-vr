#pragma once
#include "openxr_bridge_protocol.h"
#include <Windows.h>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>

// Opt-in transport for runtimes without VK_KHR_external_memory_win32.
// Both sides live in the same Wine bottle. Pixels are tightly packed BGRA8.
inline bool BMVR_IsWineOnMac()
{
    const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (!ntdll)
        return false;
    using GetHostVersion = void(__cdecl *)(const char **, const char **);
    const auto getHostVersion =
        reinterpret_cast<GetHostVersion>(GetProcAddress(ntdll, "wine_get_host_version"));
    if (!getHostVersion)
        return false;
    const char *system = nullptr;
    const char *release = nullptr;
    getHostVersion(&system, &release);
    return system && std::strcmp(system, "Darwin") == 0;
}

inline bool BMVR_UseCpuBridge()
{
    char value[8]{};
    const DWORD length = GetEnvironmentVariableA("BMVR_CPU_BRIDGE", value, sizeof(value));
    if (length == 1 && (value[0] == '0' || value[0] == '1'))
        return value[0] == '1';
    return BMVR_IsWineOnMac();
}
// Set this before the process creates its first Vulkan instance. Keep an
// explicit user override, and leave other hosts to their Vulkan loader.
inline void BMVR_ConfigureMacVulkan()
{
    if (!BMVR_IsWineOnMac())
        return;
    char advertised[32]{};
    if (!GetEnvironmentVariableA("MVK_CONFIG_API_VERSION_TO_ADVERTISE", advertised, sizeof(advertised)))
        SetEnvironmentVariableA("MVK_CONFIG_API_VERSION_TO_ADVERTISE", "4206592"); // Vulkan 1.3
}
constexpr uint32_t BMVR_CPU_MAGIC = 0x43564D42;
constexpr uint32_t BMVR_CPU_VERSION = 2;
constexpr uint32_t BMVR_CPU_MAX_DIM = 4096;
constexpr size_t BMVR_CPU_EYE_BYTES = size_t(BMVR_CPU_MAX_DIM) * BMVR_CPU_MAX_DIM * 4;
constexpr uint32_t BMVR_CPU_HANDLE_TYPE = 0x80000000u;
struct BMVR_CpuFrame
{
    uint32_t magic, version, width, height, frame;
    uint32_t reserved[3];
    L4D2VROpenXrPoseDesc pose;
    int64_t captureQpc;
    uint32_t padding[8];
    uint8_t pixels[2 * BMVR_CPU_EYE_BYTES];
};
static_assert(offsetof(BMVR_CpuFrame, pixels) == 128, "CPU bridge layout must match Win32 and x64");
static_assert(sizeof(BMVR_CpuFrame) < 0x100000000ull, "mapping size must fit a DWORD");
class BMVR_CpuMapping
{
  public:
    BMVR_CpuFrame *data = nullptr;
    ~BMVR_CpuMapping()
    {
        Close();
    }
    BMVR_CpuMapping() = default;
    BMVR_CpuMapping(const BMVR_CpuMapping &) = delete;
    BMVR_CpuMapping &operator=(const BMVR_CpuMapping &) = delete;
    bool Open(DWORD pid, bool create, uint32_t channel = 0)
    {
        if (data)
            return true;
        wchar_t name[96]{};
        swprintf_s(name, L"Local\\BMVR_CPU_v2_%lu_%u", static_cast<unsigned long>(pid), channel);
        m_map = create ? CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                            static_cast<DWORD>(sizeof(BMVR_CpuFrame)), name)
                       : OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, name);
        if (!m_map)
            return false;
        data = static_cast<BMVR_CpuFrame *>(
            MapViewOfFile(m_map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(BMVR_CpuFrame)));
        swprintf_s(name, L"Local\\BMVR_CPU_lock_v2_%lu_%u", static_cast<unsigned long>(pid), channel);
        m_mutex = CreateMutexW(nullptr, FALSE, name);
        if (!data || !m_mutex)
        {
            Close();
            return false;
        }
        return true;
    }
    bool Lock(DWORD timeout = 100)
    {
        DWORD r = WaitForSingleObject(m_mutex, timeout);
        return r == WAIT_OBJECT_0 || r == WAIT_ABANDONED;
    }
    void Unlock()
    {
        ReleaseMutex(m_mutex);
    }

  private:
    void Close()
    {
        if (data)
            UnmapViewOfFile(data);
        if (m_map)
            CloseHandle(m_map);
        if (m_mutex)
            CloseHandle(m_mutex);
        data = nullptr;
        m_map = nullptr;
        m_mutex = nullptr;
    }
    HANDLE m_map = nullptr, m_mutex = nullptr;
};

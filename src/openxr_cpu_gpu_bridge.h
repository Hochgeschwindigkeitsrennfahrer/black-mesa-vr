#pragma once
#include "openxr_bridge_protocol.h"
#include "openxr_cpu_bridge.h"
#include <Windows.h>
#include <cstddef>
#include <cstdint>
#include <cwchar>
#include <limits>

// GPU readback writes only pixel mappings. All mutable CPU metadata lives in
// this separate control mapping. Proven prerequisite: isolated v50 Win32 GPU
// image->buffer / x64 CPU reader, 24 frames, 2026-10-14.
namespace bmvr_shared_gpu
{
constexpr uint32_t Magic = 0x47564d42;
constexpr uint32_t Version = 2;
constexpr uint32_t HandleType = 0x80000001u;
constexpr uint32_t HelperCapability = 1u; // bridge v14 reservedTail0 bit.
constexpr uint32_t SlotCount = 3;
constexpr uint32_t MaxDimension = 4096;
constexpr uint64_t PrefixBytes = 128;
constexpr uint64_t MinimumAlignment = 16384;
constexpr uint64_t MaxAlignment = 65536;
constexpr uint64_t MaxAllocationBytes = PrefixBytes + 2ull * MaxDimension * MaxDimension * 4 + MaxAlignment;
enum SlotState : uint32_t
{
    Free = 0,
    GpuWriting = 1,
    Ready = 2,
    CpuReading = 3
};
struct alignas(64) Slot
{
    uint32_t state = Free, frame = 0, reserved0[2]{};
    L4D2VROpenXrPoseDesc pose{};
    int64_t captureQpc = 0;
    float fovX = 90.f, aspect = 1.f;
    float eyeTangents[2][4]{};
    uint32_t reserved1[2]{};
};
struct alignas(64) Control
{
    uint32_t magic = 0, version = Version, size = sizeof(Control), producerPid = 0;
    uint64_t nonce = 0;
    uint32_t width = 0, height = 0, rowBytes = 0, slotCount = SlotCount;
    uint64_t eyeBytes = 0, allocationBytes = 0;
    uint32_t helperPid = 0, shutdown = 0;
    Slot slots[SlotCount]{};
};
static_assert(sizeof(L4D2VROpenXrPoseDesc) == 56, "Shared pose wire layout");
static_assert(sizeof(Slot) == 128 && offsetof(Slot, pose) == 16 && offsetof(Slot, captureQpc) == 72,
              "Win32/x64 slot wire layout");
static_assert(sizeof(Control) == 448 && offsetof(Control, nonce) == 16 && offsetof(Control, slots) == 64,
              "Win32/x64 control wire layout");
inline uint32_t Load(const uint32_t &word)
{
    return L4D2VR_OpenXrAtomicLoad(word);
}
inline void Store(uint32_t &word, uint32_t value)
{
    L4D2VR_OpenXrAtomicStore(word, value);
}
inline bool Claim(uint32_t &word, uint32_t expected, uint32_t desired)
{
    return static_cast<uint32_t>(InterlockedCompareExchange(reinterpret_cast<volatile LONG *>(&word),
                                                            LONG(desired), LONG(expected))) == expected;
}
inline bool Requested()
{
    static const bool enabled = [] {
        char value[8]{};
        const DWORD n = GetEnvironmentVariableA("BMVR_SHARED_GPU_READBACK", value, sizeof(value));
        if (n == 1)
            return value[0] == '1';
        return BMVR_UseCpuBridge();
    }();
    return enabled;
}
inline bool Layout(uint32_t width, uint32_t height, uint64_t &eyeBytes, uint64_t &logicalBytes)
{
    if (!width || !height || width > MaxDimension || height > MaxDimension)
        return false;
    eyeBytes = uint64_t(width) * height * 4;
    logicalBytes = PrefixBytes + 2 * eyeBytes;
    return logicalBytes <= MaxAllocationBytes && (eyeBytes % 4) == 0;
}
inline uint64_t RoundAllocation(uint64_t bytes, uint64_t alignment)
{
    if (alignment < MinimumAlignment)
        alignment = MinimumAlignment;
    if (alignment > MaxAlignment || (alignment & (alignment - 1)) ||
        bytes > MaxAllocationBytes - (alignment - 1))
        return 0;
    return (bytes + alignment - 1) & ~(alignment - 1);
}
inline void ControlName(wchar_t (&name)[128], uint32_t pid, uint64_t nonce)
{
    swprintf_s(name, L"Local\\BMVR_GPU_v1_%lu_%llu_control", static_cast<unsigned long>(pid),
               static_cast<unsigned long long>(nonce));
}
inline void PixelName(wchar_t (&name)[128], uint32_t pid, uint64_t nonce, uint32_t slot)
{
    swprintf_s(name, L"Local\\BMVR_GPU_v1_%lu_%llu_pixels_%u", static_cast<unsigned long>(pid),
               static_cast<unsigned long long>(nonce), slot);
}
inline bool Validate(const Control &c, uint32_t pid, uint64_t nonce, uint32_t width, uint32_t height)
{
    uint64_t eyeBytes = 0, logicalBytes = 0;
    return Load(c.magic) == Magic && c.version == Version && c.size == sizeof(Control) &&
           c.producerPid == pid && c.nonce == nonce && c.width == width && c.height == height &&
           c.slotCount == SlotCount && c.rowBytes == uint64_t(width) * 4 &&
           Layout(width, height, eyeBytes, logicalBytes) && c.eyeBytes == eyeBytes &&
           c.allocationBytes >= logicalBytes && c.allocationBytes <= MaxAllocationBytes &&
           c.allocationBytes % MinimumAlignment == 0 && !Load(c.shutdown);
}
class Mapping
{
  public:
    void *data = nullptr;
    ~Mapping()
    {
        Close();
    }
    Mapping() = default;
    Mapping(const Mapping &) = delete;
    Mapping &operator=(const Mapping &) = delete;
    bool Open(const wchar_t *name, uint64_t bytes, bool create, bool readOnly = false)
    {
        if (data)
            return true;
        if (!bytes || bytes > MaxAllocationBytes || bytes > (std::numeric_limits<SIZE_T>::max)())
            return false;
        const DWORD access = readOnly ? FILE_MAP_READ : FILE_MAP_ALL_ACCESS;
        m_handle =
            create ? CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, DWORD(bytes), name)
                   : OpenFileMappingW(access, FALSE, name);
        if (!m_handle)
            return false;
        if (create && GetLastError() == ERROR_ALREADY_EXISTS)
        {
            Close();
            return false;
        }
        data = MapViewOfFile(m_handle, access, 0, 0, SIZE_T(bytes));
        if (!data)
        {
            Close();
            return false;
        }
        return true;
    }
    void Close()
    {
        if (data)
            UnmapViewOfFile(data);
        if (m_handle)
            CloseHandle(m_handle);
        data = nullptr;
        m_handle = nullptr;
    }

  private:
    HANDLE m_handle = nullptr;
};
} // namespace bmvr_shared_gpu

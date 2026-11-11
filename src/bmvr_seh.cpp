#include <Windows.h>
#include <cstdint>
#include "bmvr_seh.h"
#include "bmvr_flags.h"

namespace
{
    using tProbe = int(__cdecl*)(const volatile int*);

    __declspec(noinline) int __cdecl GuardedProbe(const volatile int* p)
    {
        int v = 0;
        BMVR_TRY
        {
            v = *p;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            v = -1;
        }
        return v;
    }

    // Deliberately without BMVR_SEH_SAVE_REGS: shows whether this compiler
    // still needs the guard. Only for the self-test.
    __declspec(noinline) int __cdecl UnguardedProbe(const volatile int* p)
    {
        int v = 0;
        __try // seh-lint: allow (self-test reference without the guard)
        {
            v = *p;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            v = -1;
        }
        return v;
    }

    struct ProbeResult
    {
        uint32_t ebx = 0, esi = 0, edi = 0;
        int ret = 0;
    };

    constexpr uint32_t kEbx = 0x5EB15EB1u, kEsi = 0x5E515E51u, kEdi = 0x5ED15ED1u;

    // Results of the asm call. Globals, so the asm addresses them absolutely
    // (not through a register it is about to overwrite).
    volatile uint32_t g_ProbeOut[4];

    // Loads known values into EBX/ESI/EDI, calls the probe on address 0 (it
    // faults and catches), and reads the return value and the three registers
    // back. The probe is only ever called from inside this asm: a plain call
    // to the unguarded probe would wipe whatever the compiler keeps in
    // EBX/ESI/EDI here (the bug under test).
    ProbeResult RunProbe(tProbe probe)
    {
        ProbeResult r;
#if defined(__clang__) && defined(_M_IX86)
        __asm__ __volatile__(
            "movl %4, %%eax\n\t"
            "pushl %%ebx\n\t"
            "pushl %%esi\n\t"
            "pushl %%edi\n\t"
            "movl $0x5EB15EB1, %%ebx\n\t"
            "movl $0x5E515E51, %%esi\n\t"
            "movl $0x5ED15ED1, %%edi\n\t"
            "pushl $0\n\t"
            "calll *%%eax\n\t"
            "addl $4, %%esp\n\t"
            "movl %%eax, %0\n\t"
            "movl %%ebx, %1\n\t"
            "movl %%esi, %2\n\t"
            "movl %%edi, %3\n\t"
            "popl %%edi\n\t"
            "popl %%esi\n\t"
            "popl %%ebx\n\t"
            : "=m"(g_ProbeOut[0]), "=m"(g_ProbeOut[1]), "=m"(g_ProbeOut[2]), "=m"(g_ProbeOut[3])
            : "m"(probe)
            : "eax", "ecx", "edx", "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7",
              "memory", "cc");
        r.ret = static_cast<int>(g_ProbeOut[0]);
        r.ebx = g_ProbeOut[1];
        r.esi = g_ProbeOut[2];
        r.edi = g_ProbeOut[3];
#else
        r.ret = probe(nullptr);
        r.ebx = kEbx;
        r.esi = kEsi;
        r.edi = kEdi;
#endif
        return r;
    }

    bool Intact(const ProbeResult& r)
    {
        return r.ebx == kEbx && r.esi == kEsi && r.edi == kEdi;
    }
}

namespace bmvr::seh
{
    bool SelfTest()
    {
        const ProbeResult guarded = RunProbe(&GuardedProbe);
        const ProbeResult bare = RunProbe(&UnguardedProbe);
        const bool ok = guarded.ret == -1 && Intact(guarded);
        Log("SEH register self-test: %s (guarded fault caught=%d ebx=%08X esi=%08X edi=%08X; "
            "without the guard: %s, ebx=%08X esi=%08X edi=%08X)",
            ok ? "OK" : "FAILED, caught faults can crash the caller",
            guarded.ret == -1 ? 1 : 0, guarded.ebx, guarded.esi, guarded.edi,
            Intact(bare) ? "registers kept (guard not needed by this compiler)" : "registers lost (guard needed)",
            bare.ebx, bare.esi, bare.edi);
        return ok;
    }

    bool IsReadable(const void* p, size_t size)
    {
        if (!p || size == 0)
            return false;
        uintptr_t at = reinterpret_cast<uintptr_t>(p);
        const uintptr_t end = at + size;
        if (end < at)
            return false;
        constexpr DWORD kReadable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY
            | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
        while (at < end)
        {
            MEMORY_BASIC_INFORMATION mbi{};
            if (VirtualQuery(reinterpret_cast<const void*>(at), &mbi, sizeof(mbi)) != sizeof(mbi))
                return false;
            if (mbi.State != MEM_COMMIT || !(mbi.Protect & kReadable) || (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)))
                return false;
            const uintptr_t regionEnd = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
            if (regionEnd <= at)
                return false;
            at = regionEnd;
        }
        return true;
    }
}

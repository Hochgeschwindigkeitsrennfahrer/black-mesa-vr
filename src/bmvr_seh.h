#pragma once

// Structured exception guards (__try/__except) for the 32-bit clang-cl build.
//
// Use BMVR_TRY instead of a bare __try. The build fails on a bare __try in
// src/ (tools/check_seh_guards.cmake).
//
// Why: on x86, clang reaches an __except block through the CRT's
// _except_handler3, which leaves EBX/ESI/EDI holding its own values. MSVC
// saves those three in the prologue of every function with a __try; clang
// saves only the ones the function itself uses. A guard that catches a fault
// then returns to its caller with EBX/ESI/EDI wiped, and the caller crashes
// later (bloom-off crash 2026-11-11: ESI = 0 in the material param scan).
//
// BMVR_SEH_SAVE_REGS tells the compiler the function overwrites all three, so
// it saves them in the prologue and the epilogue restores them on both the
// normal and the __except path. It emits no instructions. One use anywhere in
// a function covers the whole function. Checked at startup by
// bmvr::seh::SelfTest (log line "SEH register self-test").
//
// The other clang-cl SEH flags live in cmake/clang-cl-xwin-x86.cmake.

#if defined(__clang__) && defined(_M_IX86)
#define BMVR_SEH_SAVE_REGS() __asm__ __volatile__("" ::: "ebx", "esi", "edi")
#else
#define BMVR_SEH_SAVE_REGS() ((void)0)
#endif

#define BMVR_TRY BMVR_SEH_SAVE_REGS(); __try

namespace bmvr::seh
{
    // Calls a guarded probe that faults, from code holding known values in
    // EBX/ESI/EDI, and logs whether they survived. Also logs whether a probe
    // without BMVR_SEH_SAVE_REGS loses them (= the guard is still needed).
    // Returns false when the guarded probe lost them.
    bool SelfTest();

    // True when [p, p + size) is committed readable memory. No fault, no SEH.
    bool IsReadable(const void* p, size_t size);
}

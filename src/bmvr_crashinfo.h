#pragma once

// Crash evidence (user 2026-11-20: a save load after a map transition killed
// the game with no fault in bmvr_fault.txt). Minidumps for fatal exceptions,
// a run header and an exit line in bmvr_fault.txt.
namespace bmvr::crashinfo
{
    // DLL attach: unhandled-exception filter (minidump + bmvr_crash.txt line).
    void Install();
    // Fatal exception codes (heap corruption, fail-fast, stack overflow...)
    // seen by the first-chance handler: write a minidump once per process.
    bool IsFatalCode(unsigned long code);
    void DumpNow(void* exceptionPointers, const char* why);
    // DLL detach: one log line (normal exit vs. killed: no line).
    void NoteDetach(bool processExit);
}

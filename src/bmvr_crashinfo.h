#pragma once

// Crash evidence (user 2026-11-20: a save load after a map transition killed
// the game with no fault in bmvr_fault.txt). Minidumps for fatal exceptions,
// heap validation at chosen points, an exit line.
namespace bmvr::crashinfo
{
    // DLL attach: unhandled-exception filter (minidump + bmvr_crash.txt line).
    void Install();
    // Fatal exception codes (heap corruption, fail-fast, stack overflow...)
    // seen by the first-chance handler: write a minidump once per process.
    bool IsFatalCode(unsigned long code);
    void DumpNow(void* exceptionPointers, const char* why);
    // HeapValidate on every process heap, logged with where and the time
    // taken. Returns false if any heap is corrupt.
    bool CheckHeaps(const char* where);
    // Checks after a level start (1, 3, 6, 10 s), on a worker thread.
    void ScheduleHeapChecks(const char* map);
    // DLL detach: one log line (normal exit vs. killed: no line).
    void NoteDetach(bool processExit);
}

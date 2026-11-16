#pragma once
// PerfLog wait profiler (see bmvr_sampler.cpp). Only active with PerfLog.
namespace bmvr::sampler
{
    void NoteMainThread();    // RenderView thread; starts the sampler once
    void NoteRenderThread();  // thread issuing the d3d9 draws
    void Report(int frames);  // top wait sites per thread, from prof::Report
    // CPU sampler (cvar bmvr_cpu_sample): 0 = main thread (RenderView), 1 = render thread (Present).
    void CpuNote(int which);
    bool CpuSampleStart(double seconds);
}

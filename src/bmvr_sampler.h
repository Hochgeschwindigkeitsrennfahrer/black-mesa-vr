#pragma once
// PerfLog wait profiler (see bmvr_sampler.cpp). Only active with PerfLog.
namespace bmvr::sampler
{
    void NoteMainThread();    // RenderView thread; starts the sampler once
    void NoteRenderThread();  // thread issuing the d3d9 draws
    void Report(int frames);  // top wait sites per thread, from prof::Report
}

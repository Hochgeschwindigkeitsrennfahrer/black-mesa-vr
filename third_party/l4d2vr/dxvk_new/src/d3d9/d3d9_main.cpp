#include "../dxvk/dxvk_instance.h"

#include "d3d9_interface.h"
#include "d3d9_shader_validator.h"
#include "d3d9_reshade_vr.h"

#include "d3d9_annotation.h"
#include "../../../../../src/openxr_cpu_bridge.h"

namespace {
  // Wine/Darwin execute-fault workaround, scoped to this process.
  void BMVR_ConfigureDepOnce() {
    static const bool configured = [] {
      using Query = LONG (NTAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);
      using Set = LONG (NTAPI*)(HANDLE, ULONG, PVOID, ULONG);
      HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
      auto query = reinterpret_cast<Query>(GetProcAddress(ntdll, "NtQueryInformationProcess"));
      auto set = reinterpret_cast<Set>(GetProcAddress(ntdll, "NtSetInformationProcess"));
      char value[8]{};
      const DWORD envSize = GetEnvironmentVariableA("BMVR_ENABLE_DEP", value, sizeof(value));
      const bool enabled = envSize == 1 ? value[0] == '1' : BMVR_IsWineOnMac();
      ULONG before = 0, after = 0;
      LONG qs = query ? query(GetCurrentProcess(), 34, &before, sizeof(before), nullptr) : LONG(0xc0000002u);
      LONG ss = 0;
      if (enabled && qs >= 0 && (before & 2u)) {
        ULONG desired = (before & ~2u) | 1u;
        ss = set ? set(GetCurrentProcess(), 34, &desired, sizeof(desired)) : LONG(0xc0000002u);
      }
      LONG vs = query ? query(GetCurrentProcess(), 34, &after, sizeof(after), nullptr) : LONG(0xc0000002u);
      dxvk::Logger::warn(dxvk::str::format("[BMVR DEP v27] pid=", GetCurrentProcessId(),
        " gate=", enabled, " queryStatus=", qs, " before=", before,
        " setStatus=", ss, " verifyStatus=", vs, " after=", after,
        " permanentBitAdded=0"));
      return true;
    }();
    (void)configured;
  }
}

class D3DFE_PROCESSVERTICES;
using PSGPERRORID = UINT;

namespace dxvk {
  Logger Logger::s_instance("d3d9.log");
  D3D9GlobalAnnotationList D3D9GlobalAnnotationList::s_instance;

  HRESULT CreateD3D9(
          bool           Extended,
          IDirect3D9Ex** ppDirect3D9Ex,
    const D3D9ON12_ARGS* pOverrideList,
          uint32_t       OverrideCount) {
    BMVR_ConfigureMacVulkan();
    BMVR_ConfigureDepOnce();
    if (!ppDirect3D9Ex)
      return D3DERR_INVALIDCALL;

    *ppDirect3D9Ex = ref(new D3D9InterfaceEx(Extended, pOverrideList, OverrideCount));
    return D3D_OK;
  }
}


extern "C" {

  DLLEXPORT IDirect3D9* __stdcall Direct3DCreate9(UINT nSDKVersion) {
    dxvk::D3D9ReShadeVrPrepareConfiguration();

    IDirect3D9Ex* pDirect3D = nullptr;
    dxvk::CreateD3D9(false, &pDirect3D, nullptr, 0);

    return pDirect3D;
  }

  DLLEXPORT HRESULT __stdcall Direct3DCreate9Ex(UINT nSDKVersion, IDirect3D9Ex** ppDirect3D9Ex) {
    dxvk::D3D9ReShadeVrPrepareConfiguration();
    return dxvk::CreateD3D9(true, ppDirect3D9Ex, nullptr, 0);
  }

  DLLEXPORT int __stdcall D3DPERF_BeginEvent(D3DCOLOR col, LPCWSTR wszName) {
    return dxvk::D3D9GlobalAnnotationList::Instance().BeginEvent(col, wszName);
  }

  DLLEXPORT int __stdcall D3DPERF_EndEvent(void) {
    return dxvk::D3D9GlobalAnnotationList::Instance().EndEvent();
  }

  DLLEXPORT void __stdcall D3DPERF_SetMarker(D3DCOLOR col, LPCWSTR wszName) {
    dxvk::D3D9GlobalAnnotationList::Instance().SetMarker(col, wszName);
  }

  DLLEXPORT void __stdcall D3DPERF_SetRegion(D3DCOLOR col, LPCWSTR wszName) {
    dxvk::D3D9GlobalAnnotationList::Instance().SetRegion(col, wszName);
  }

  DLLEXPORT BOOL __stdcall D3DPERF_QueryRepeatFrame(void) {
    return dxvk::D3D9GlobalAnnotationList::Instance().QueryRepeatFrame();
  }

  DLLEXPORT void __stdcall D3DPERF_SetOptions(DWORD dwOptions) {
    dxvk::D3D9GlobalAnnotationList::Instance().SetOptions(dwOptions);
  }

  DLLEXPORT DWORD __stdcall D3DPERF_GetStatus(void) {
    return dxvk::D3D9GlobalAnnotationList::Instance().GetStatus();
  }


  DLLEXPORT void __stdcall DebugSetMute(void) {
  }

  DLLEXPORT int __stdcall DebugSetLevel(void) {
    return 0;
  }

  // Processor Specific Geometry Pipeline
  // for P3 SIMD/AMD 3DNow.

  DLLEXPORT void __stdcall PSGPError(D3DFE_PROCESSVERTICES* a, PSGPERRORID b, UINT c) {
  }

  DLLEXPORT void __stdcall PSGPSampleTexture(D3DFE_PROCESSVERTICES* a, UINT b, float(*const c)[4], UINT d, float(*const e)[4]) {
  }

  DLLEXPORT dxvk::D3D9ShaderValidator* __stdcall Direct3DShaderValidatorCreate9(void) {
    return ref(new dxvk::D3D9ShaderValidator());
  }

  DLLEXPORT int __stdcall Direct3D9EnableMaximizedWindowedModeShim(UINT a) {
    return 0;
  }

  DLLEXPORT void __stdcall DXVK_RegisterAnnotation(IDXVKUserDefinedAnnotation* annotation) {
    dxvk::D3D9GlobalAnnotationList::Instance().RegisterAnnotator(annotation);
  }

  DLLEXPORT void __stdcall DXVK_UnRegisterAnnotation(IDXVKUserDefinedAnnotation* annotation) {
    dxvk::D3D9GlobalAnnotationList::Instance().UnregisterAnnotator(annotation);
  }

  DLLEXPORT void __stdcall Direct3D9ForceHybridEnumeration(UINT uHybrid) {
  }

  DLLEXPORT IDirect3D9* __stdcall Direct3DCreate9On12(UINT sdk_version, D3D9ON12_ARGS* override_list, UINT override_entry_count) {
    dxvk::Logger::warn("Direct3DCreate9On12: 9On12 functionality is unimplemented.");

    IDirect3D9Ex* pDirect3D = nullptr;
    dxvk::CreateD3D9(false, &pDirect3D, override_list, override_entry_count);

    return pDirect3D;
  }

  DLLEXPORT HRESULT __stdcall Direct3DCreate9On12Ex(UINT sdk_version, D3D9ON12_ARGS* override_list, UINT override_entry_count, IDirect3D9Ex** output) {
    dxvk::Logger::warn("Direct3DCreate9On12Ex: 9On12 functionality is unimplemented.");
    return dxvk::CreateD3D9(true, output, override_list, override_entry_count);
  }

}

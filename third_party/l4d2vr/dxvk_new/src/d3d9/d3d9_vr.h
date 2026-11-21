#pragma once

#include <d3d9.h>

#define VK_USE_PLATFORM_WIN32_KHR 1
#include <vulkan/vulkan.h>
#undef VK_USE_PLATFORM_WIN32_KHR

class IDirect3DVR9;
class D3D9DeviceEx;
class SharedTextureHolder;
class VR;
inline IDirect3DVR9* g_D3DVR9;
// Qt/ANGLE may create an auxiliary 1x1 D3D9 device. Never bind VR to it.
inline IDirect3DDevice9* g_BmvrSourceDevice9 = nullptr;

struct D3D9_TEXTURE_VR_DESC {
  uint64_t         Image;
  VkDevice         Device;
  VkPhysicalDevice PhysicalDevice;
  VkInstance       Instance;
  VkQueue          Queue;
  uint32_t         QueueFamilyIndex;

  uint32_t         Width;
  uint32_t         Height;
  VkFormat         Format;
  uint32_t         SampleCount;

  // KMT export for L4D2VR's x64 OpenXR helper (OPAQUE_WIN32_KMT).
  uint64_t         SharedHandle;
  uint32_t         SharedHandleType;
  uint32_t         SharedHandleValid;
};

struct D3D9_HOST_READBACK_LAYOUT {
  uint32_t Width = 0, Height = 0;
  uint64_t EyeBytes = 0, AllocationBytes = 0;
};

MIDL_INTERFACE("ec2dc113-ff34-4b93-b93d-0ea8b88f4250")
IDirect3DHostReadback9 : public IUnknown { };

MIDL_INTERFACE("7e272b32-a49c-46c7-b1a4-ef52936bec87")
IDirect3DVR9 : public IUnknown {
  virtual HRESULT STDMETHODCALLTYPE GetVRDesc(IDirect3DSurface9* pSurface, D3D9_TEXTURE_VR_DESC* pDesc) = 0;
  virtual HRESULT STDMETHODCALLTYPE TransferSurface(IDirect3DSurface9* pSurface, BOOL waitResourceIdle) = 0;
  virtual HRESULT STDMETHODCALLTYPE LockDevice() = 0;
  virtual HRESULT STDMETHODCALLTYPE UnlockDevice() = 0;
  virtual HRESULT STDMETHODCALLTYPE LockSubmissionQueue() = 0;
  virtual HRESULT STDMETHODCALLTYPE LockPreparedSubmissionQueue() = 0;
  virtual HRESULT STDMETHODCALLTYPE UnlockSubmissionQueue() = 0;
  virtual HRESULT STDMETHODCALLTYPE DrawQueuedEyeSubmitOverlays(VR* vr) = 0;
  virtual HRESULT STDMETHODCALLTYPE WaitDeviceIdle() = 0;
  virtual HRESULT STDMETHODCALLTYPE GetBackBufferData(SharedTextureHolder* backBufferData) = 0;
  virtual HRESULT STDMETHODCALLTYPE BeginSourceFrameOwnership() = 0;
  virtual HRESULT STDMETHODCALLTYPE EndSourceFrameOwnership() = 0;
  // Diagnostics need the live D3D9 device even when -nohmd skips all VR
  // render-target allocation.  Keep this at the end of the private interface
  // so existing method indices remain stable.
  virtual HRESULT STDMETHODCALLTYPE GetD3DDevice(IDirect3DDevice9** ppDevice) = 0;
  // Hand every recorded D3D9 command to the CS thread for GPU submission
  // without waiting for it (no CS drain, no device idle). Used by the OpenXR
  // deferred eye publish so a D3DQUERYTYPE_EVENT issued after the eye copies
  // is guaranteed to reach the GPU this frame. Appended last: method indices
  // above stay stable.
  virtual HRESULT STDMETHODCALLTYPE FlushCommands() = 0;
  // Appended v50 private API. Target owns imported pixel mapping through DXVK
  // command-list retirement; caller must prove its EVENT complete before idle.
  virtual HRESULT STDMETHODCALLTYPE CreateHostReadbackPair(const wchar_t* name, uint32_t width, uint32_t height,
      D3D9_HOST_READBACK_LAYOUT* layout, IDirect3DHostReadback9** target) = 0;
  virtual HRESULT STDMETHODCALLTYPE QueueHostReadbackPair(IDirect3DSurface9* left, IDirect3DSurface9* right,
      IDirect3DHostReadback9* target) = 0;
  virtual HRESULT STDMETHODCALLTYPE IsHostReadbackIdle(IDirect3DHostReadback9* target) = 0;
};

#ifdef _MSC_VER
struct __declspec(uuid("7e272b32-a49c-46c7-b1a4-ef52936bec87")) IDirect3DVR9;
#else
__CRT_UUID_DECL(IDirect3DVR9, 0x7e272b32, 0xa49c, 0x46c7, 0xb1, 0xa4, 0xef, 0x52, 0x93, 0x6b, 0xec, 0x87);
#endif

HRESULT __stdcall Direct3DCreateVRImpl(IDirect3DDevice9* pDevice, IDirect3DVR9** pInterface);

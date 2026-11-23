#include "../dxvk/dxvk_include.h"

#include "d3d9_vr.h"

#include "d3d9_include.h"
#include "d3d9_surface.h"

#include "d3d9_device.h"

#include "L4D2VR/game.h"
#include "L4D2VR/vr.h"

#include <cstring>
#include <memory>
#include "openxr_cpu_gpu_bridge.h"

namespace dxvk {

  struct HostReadbackOwner {
    bmvr_shared_gpu::Mapping mapping;
    Rc<vk::DeviceFn> vk;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    ~HostReadbackOwner() {
      // Does not re-enter the DXVK allocator. The imported allocation retains
      // this owner until its final command-list reference is retired.
      if (buffer) vk->vkDestroyBuffer(vk->device(), buffer, nullptr);
      if (memory) vk->vkFreeMemory(vk->device(), memory, nullptr);
      // mapping closes AFTER native VkBuffer/VkDeviceMemory destruction.
    }
  };

  class HostReadbackTarget final : public ComObjectClamp<IDirect3DHostReadback9> {
  public:
    D3D9DeviceEx* device = nullptr;
    Rc<DxvkBuffer> buffer;
    D3D9_HOST_READBACK_LAYOUT layout{};
    ~HostReadbackTarget() {
      buffer = nullptr; // Its allocator belongs to the parent D3D9 device.
      if (device) device->Release();
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) {
      if (!object) return E_POINTER;
      *object = nullptr;
      if (riid != __uuidof(IUnknown) && riid != __uuidof(IDirect3DHostReadback9)) return E_NOINTERFACE;
      *object = ref(this);
      return S_OK;
    }
  };

  class D3D9VR final : public ComObjectClamp<IDirect3DVR9> {

  public:

    D3D9VR(IDirect3DDevice9* pDevice)
      : m_device(static_cast<D3D9DeviceEx*>(pDevice)) {
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(
            REFIID riid,
            void** ppvObject) {
      if (ppvObject == nullptr)
        return E_POINTER;

      *ppvObject = nullptr;

      if (riid == __uuidof(IUnknown)
       || riid == __uuidof(IDirect3DVR9)) {
        *ppvObject = ref(this);
        return S_OK;
      }

      Logger::warn("D3D9VR::QueryInterface: Unknown interface query");
      Logger::warn(str::format(riid));
      return E_NOINTERFACE;
    }

    HRESULT STDMETHODCALLTYPE GetVRDesc(
            IDirect3DSurface9*   pSurface,
            D3D9_TEXTURE_VR_DESC* pDesc) {
      if (unlikely(pSurface == nullptr || pDesc == nullptr))
        return D3DERR_INVALIDCALL;

      D3D9Surface* surface = static_cast<D3D9Surface*>(pSurface);

      const auto* tex = surface->GetCommonTexture();

      const auto& desc = tex->Desc();
      const auto& image = tex->GetImage();
      if (unlikely(image == nullptr))
        return D3DERR_INVALIDCALL;

      const auto& device = tex->Device()->GetDXVKDevice();

      // I don't know why the image randomly is a uint64_t in OpenVR.
      pDesc->Image = uint64_t(image->handle());
      pDesc->Device = device->handle();
      pDesc->PhysicalDevice = device->adapter()->handle();
      pDesc->Instance = device->instance()->handle();
      pDesc->Queue = device->queues().graphics.queueHandle;
      pDesc->QueueFamilyIndex = device->queues().graphics.queueIndex;

      pDesc->Width = desc->Width;
      pDesc->Height = desc->Height;
      pDesc->Format = tex->GetFormatMapping().FormatColor;
      pDesc->SampleCount = uint32_t(image->info().sampleCount);
      HANDLE sharedHandle = image->sharedHandle();
      pDesc->SharedHandle = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(sharedHandle));
      pDesc->SharedHandleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_KMT_BIT;
      pDesc->SharedHandleValid = (sharedHandle != nullptr && sharedHandle != INVALID_HANDLE_VALUE) ? 1u : 0u;

      return D3D_OK;
    }

    HRESULT STDMETHODCALLTYPE TransferSurface(
            IDirect3DSurface9* pSurface,
            BOOL               waitResourceIdle) {
      if (unlikely(pSurface == nullptr))
        return D3DERR_INVALIDCALL;

      D3D9DeviceLock lock = m_device->LockDeviceExclusive();

      auto* tex = static_cast<D3D9Surface*>(pSurface)->GetCommonTexture();
      const auto& image = tex->GetImage();

      VR* vr = (g_Game != nullptr) ? g_Game->m_VR : nullptr;
      if (vr && vr->m_ReShadeVRCompat) {
        IDirect3DSurface9* rightEyeSurface = vr->m_D9RightEyeSubmitSurface
          ? vr->m_D9RightEyeSubmitSurface
          : vr->m_D9RightEyeSurface;
        if (pSurface == rightEyeSurface)
          m_device->UpdateReShadeVrDepthAtlas(vr);
      }

      VkImageSubresourceRange subresources = {
        VK_IMAGE_ASPECT_COLOR_BIT,
        0, image->info().mipLevels,
        0, image->info().numLayers
      };

      const VkImageLayout readableLayout =
        (g_Game && g_Game->m_VR && g_Game->m_VR->m_OpenXrHelperBridgeActive)
          ? VK_IMAGE_LAYOUT_GENERAL
          : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;

      m_device->TransformImage(
        tex, &subresources,
        image->info().layout,
        readableLayout);

      // This wait may need to be on all Faces and Mip Levels (2 loops).
      if (waitResourceIdle)
        m_device->WaitForResource(*image, tex->GetMappingBufferSequenceNumber(0u), D3DLOCK_READONLY);

      return D3D_OK;
    }

    HRESULT STDMETHODCALLTYPE LockDevice() {
      m_lock = m_device->LockDeviceExclusive();
      return D3D_OK;
    }

    HRESULT STDMETHODCALLTYPE UnlockDevice() {
      m_lock = D3D9DeviceLock();
      return D3D_OK;
    }

    HRESULT STDMETHODCALLTYPE LockSubmissionQueue() {
      // Drain DXVK's CPU command stream while no other D3D9 call can append to it,
      // then take the queue's native external-synchronization gate. Once acquired,
      // the device lock can be released: Source may record the next frame, but DXVK's
      // submit thread cannot touch VkQueue until OpenVR has submitted the stereo pair.
      D3D9DeviceLock lock = m_device->LockDeviceExclusive();
      m_device->Flush();
      m_device->SynchronizeCsThread(DxvkCsThread::SynchronizeAll);
      m_device->GetDXVKDevice()->lockSubmission();
      return D3D_OK;
    }

    HRESULT STDMETHODCALLTYPE LockPreparedSubmissionQueue() {
      // The producer has already flushed and synchronized the D3D9 command stream
      // before publishing this texture generation. Only serialize native VkQueue
      // access here; doing another CS-thread drain on every VR submit defeats queued
      // rendering and puts the render-thread backlog on the Present thread.
      m_device->GetDXVKDevice()->lockSubmission();
      return D3D_OK;
    }

    HRESULT STDMETHODCALLTYPE DrawQueuedEyeSubmitOverlays(VR* vr) {
      if (unlikely(vr == nullptr))
        return D3DERR_INVALIDCALL;

      // The caller owns the VR texture mutex and the recursive D3D9 device lock.
      // Keep these draws in the same copy -> decorate -> transition transaction.
      m_device->DrawQueuedEyeSubmitOverlays(vr);
      return D3D_OK;
    }

    HRESULT STDMETHODCALLTYPE UnlockSubmissionQueue() {
      m_device->GetDXVKDevice()->unlockSubmission();
      return D3D_OK;
    }

    HRESULT STDMETHODCALLTYPE WaitDeviceIdle() {
      // This can be called from the VR/Present path while Source's queued material
      // thread is active. DXVK's D3D9 command chunk is device-owned mutable state, so
      // flushing/synchronizing without the same device lock used by draw calls can race
      // DrawIndexedPrimitive and leave m_csChunk null/moved.
      D3D9DeviceLock lock = m_device->LockDeviceExclusive();

      m_device->Flush();
      // Not clear if we need all here, perhaps...
      m_device->SynchronizeCsThread(DxvkCsThread::SynchronizeAll);
      m_device->GetDXVKDevice()->waitForIdle();
      return D3D_OK;
    }

    HRESULT STDMETHODCALLTYPE GetBackBufferData(SharedTextureHolder* backBufferData) {
      if (unlikely(backBufferData == nullptr))
        return D3DERR_INVALIDCALL;

      IDirect3DSurface9* backBufferSurface = nullptr;

      HRESULT res = m_device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backBufferSurface);
      if (FAILED(res) || backBufferSurface == nullptr)
        return FAILED(res) ? res : D3DERR_INVALIDCALL;

      D3D9_TEXTURE_VR_DESC textureDesc = {};

      res = GetVRDesc(backBufferSurface, &textureDesc);

      if (backBufferSurface != nullptr)
        backBufferSurface->Release();

      if (FAILED(res))
        return res;

      std::memcpy(&backBufferData->m_VulkanData, &textureDesc, sizeof(vr::VRVulkanTextureData_t));
      backBufferData->m_VRTexture.handle = &backBufferData->m_VulkanData;
      backBufferData->m_VRTexture.eColorSpace = vr::ColorSpace_Auto;
      backBufferData->m_VRTexture.eType = vr::TextureType_Vulkan;

      return res;
    }

    HRESULT STDMETHODCALLTYPE BeginSourceFrameOwnership() {
      return m_device->BeginSourceFrameOwnership() ? D3D_OK : D3DERR_INVALIDCALL;
    }

    HRESULT STDMETHODCALLTYPE EndSourceFrameOwnership() {
      return m_device->EndSourceFrameOwnership() ? D3D_OK : D3DERR_INVALIDCALL;
    }

    HRESULT STDMETHODCALLTYPE GetD3DDevice(IDirect3DDevice9** ppDevice) {
      if (unlikely(ppDevice == nullptr))
        return E_POINTER;

      *ppDevice = m_device;
      m_device->AddRef();
      return D3D_OK;
    }

    HRESULT STDMETHODCALLTYPE FlushCommands() {
      // Same lock discipline as WaitDeviceIdle: the CS chunk is device-owned
      // mutable state. Flush only; the CS thread submits asynchronously.
      D3D9DeviceLock lock = m_device->LockDeviceExclusive();
      m_device->Flush();
      return D3D_OK;
    }

    HRESULT STDMETHODCALLTYPE CreateHostReadbackPair(const wchar_t* name, uint32_t width, uint32_t height,
        D3D9_HOST_READBACK_LAYOUT* layout, IDirect3DHostReadback9** target) {
      if (!layout || !target) return E_POINTER;
      *layout = {}; *target = nullptr;
      uint64_t eyeBytes = 0, logicalBytes = 0;
      if (!bmvr_shared_gpu::Requested() || !name || !*name || !bmvr_shared_gpu::Layout(width, height, eyeBytes, logicalBytes))
        return D3DERR_NOTAVAILABLE;
      D3D9DeviceLock lock = m_device->LockDeviceExclusive();
      const auto device = m_device->GetDXVKDevice();
      const auto vk = device->vkd();
      const auto vi = device->vki();
      if (!vk->vkGetMemoryHostPointerPropertiesEXT || !vi->vkGetPhysicalDeviceExternalBufferProperties)
        return D3DERR_NOTAVAILABLE;
      try {
        VkPhysicalDeviceExternalMemoryHostPropertiesEXT host{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT};
        VkPhysicalDeviceProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2}; properties.pNext = &host;
        vi->vkGetPhysicalDeviceProperties2(device->adapter()->handle(), &properties);
        if (!host.minImportedHostPointerAlignment || host.minImportedHostPointerAlignment > bmvr_shared_gpu::MaxAlignment)
          return D3DERR_NOTAVAILABLE;
        VkPhysicalDeviceExternalBufferInfo externalInfo{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_BUFFER_INFO};
        externalInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        externalInfo.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
        VkExternalBufferProperties externalProperties{VK_STRUCTURE_TYPE_EXTERNAL_BUFFER_PROPERTIES};
        vi->vkGetPhysicalDeviceExternalBufferProperties(device->adapter()->handle(), &externalInfo, &externalProperties);
        const auto features = externalProperties.externalMemoryProperties.externalMemoryFeatures;
        if (!(features & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT) || (features & VK_EXTERNAL_MEMORY_FEATURE_DEDICATED_ONLY_BIT))
          return D3DERR_NOTAVAILABLE;
        auto owner = std::make_shared<HostReadbackOwner>(); owner->vk = vk;
        VkExternalMemoryBufferCreateInfo external{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO};
        external.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
        VkBufferCreateInfo create{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; create.pNext = &external;
        create.size = logicalBytes; create.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT; create.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vk->vkCreateBuffer(vk->device(), &create, nullptr, &owner->buffer) != VK_SUCCESS) return D3DERR_NOTAVAILABLE;
        VkMemoryRequirements requirements{}; vk->vkGetBufferMemoryRequirements(vk->device(), owner->buffer, &requirements);
        const uint64_t allocationBytes = bmvr_shared_gpu::RoundAllocation(std::max<uint64_t>(logicalBytes, requirements.size), host.minImportedHostPointerAlignment);
        if (!allocationBytes || !owner->mapping.Open(name, allocationBytes, true)) return D3DERR_OUTOFVIDEOMEMORY;
        const auto base = reinterpret_cast<uintptr_t>(owner->mapping.data);
        if (base % bmvr_shared_gpu::MinimumAlignment || base % host.minImportedHostPointerAlignment) return D3DERR_NOTAVAILABLE;
        // Immutable prefix; mutable frame/pose/state stay in the control mapping.
        std::memset(owner->mapping.data, 0, size_t(bmvr_shared_gpu::PrefixBytes));
        VkMemoryHostPointerPropertiesEXT pointer{VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT};
        if (vk->vkGetMemoryHostPointerPropertiesEXT(vk->device(), VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT,
            owner->mapping.data, &pointer) != VK_SUCCESS) return D3DERR_NOTAVAILABLE;
        VkPhysicalDeviceMemoryProperties memory{}; vi->vkGetPhysicalDeviceMemoryProperties(device->adapter()->handle(), &memory);
        const uint32_t candidates = requirements.memoryTypeBits & pointer.memoryTypeBits;
        uint32_t selected = ~0u; unsigned bestScore = 0;
        for (uint32_t i = 0; i < memory.memoryTypeCount; ++i) {
          const auto flags = memory.memoryTypes[i].propertyFlags;
          const auto required = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
          if (!(candidates & (1u << i)) || (flags & required) != required) continue;
          const unsigned score = 1u + ((flags & VK_MEMORY_PROPERTY_HOST_CACHED_BIT) ? 2u : 0u);
          if (score > bestScore) { selected = i; bestScore = score; }
        }
        if (selected == ~0u) return D3DERR_NOTAVAILABLE;
        VkImportMemoryHostPointerInfoEXT imported{VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT};
        imported.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT; imported.pHostPointer = owner->mapping.data;
        VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; allocate.pNext = &imported;
        allocate.allocationSize = allocationBytes; allocate.memoryTypeIndex = selected;
        if (vk->vkAllocateMemory(vk->device(), &allocate, nullptr, &owner->memory) != VK_SUCCESS
            || vk->vkBindBufferMemory(vk->device(), owner->buffer, owner->memory, 0) != VK_SUCCESS) return D3DERR_NOTAVAILABLE;
        DxvkBufferCreateInfo info{}; info.size = logicalBytes; info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        info.stages = VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_HOST_BIT;
        info.access = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_HOST_READ_BIT;
        DxvkBufferImportInfo importInfo{}; importInfo.buffer = owner->buffer; importInfo.mapPtr = owner->mapping.data; importInfo.owner = owner;
        auto result = std::make_unique<HostReadbackTarget>();
        result->device = m_device;
        result->device->AddRef();
        result->buffer = device->importBuffer(info, importInfo, memory.memoryTypes[selected].propertyFlags);
        result->layout = { width, height, eyeBytes, allocationBytes };
        *layout = result->layout;
        result->AddRef();
        *target = result.release();
        return D3D_OK;
      } catch (const std::exception& error) {
        Logger::warn(str::format("BMVR shared GPU readback unavailable: ", error.what()));
        return D3DERR_NOTAVAILABLE;
      }
    }

    HRESULT STDMETHODCALLTYPE QueueHostReadbackPair(IDirect3DSurface9* left, IDirect3DSurface9* right,
        IDirect3DHostReadback9* target) {
      if (!target) return D3DERR_INVALIDCALL;
      auto* resource = static_cast<HostReadbackTarget*>(target);
      if (resource->device != m_device) return D3DERR_INVALIDCALL;
      return m_device->CopyHostReadbackPair(left, right, resource->buffer,
        resource->layout.Width, resource->layout.Height, resource->layout.EyeBytes);
    }

    HRESULT STDMETHODCALLTYPE IsHostReadbackIdle(IDirect3DHostReadback9* target) {
      if (!target) return D3DERR_INVALIDCALL;
      auto* resource = static_cast<HostReadbackTarget*>(target);
      // Old-device targets can still be retired after a game device change;
      // each target keeps its own parent alive and this check is read-only.
      return resource->buffer->isInUse(DxvkAccess::Read) ? S_FALSE : S_OK;
    }

  private:

    D3D9DeviceEx* m_device;
    D3D9DeviceLock m_lock;

  };

}

HRESULT __stdcall Direct3DCreateVRImpl(IDirect3DDevice9* pDevice, IDirect3DVR9** pInterface) {
  if (pDevice == nullptr || pInterface == nullptr)
    return D3DERR_INVALIDCALL;

  *pInterface = new dxvk::D3D9VR(pDevice);

  return D3D_OK;
}

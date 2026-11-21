#include "dxvk_device.h"

namespace dxvk {
  
  DxvkUnboundResources::DxvkUnboundResources(DxvkDevice* dev)
  : m_device(dev) {

  }
  
  
  DxvkUnboundResources::~DxvkUnboundResources() {
    
  }


  VkBuffer DxvkUnboundResources::bufferHandle() {
    VkBuffer buffer = m_bufferHandle.load();

    if (likely(buffer != VK_NULL_HANDLE))
      return buffer;

    std::lock_guard lock(m_mutex);
    buffer = m_bufferHandle.load();

    if (buffer)
      return buffer;

    m_buffer = createBuffer();
    buffer = m_buffer->getSliceHandle().handle;

    m_bufferHandle.store(buffer, std::memory_order_release);
    return buffer;
  }


  VkSampler DxvkUnboundResources::samplerHandle() {
    VkSampler sampler = m_samplerHandle.load();

    if (likely(sampler != VK_NULL_HANDLE))
      return sampler;

    std::lock_guard lock(m_mutex);
    sampler = m_samplerHandle.load();

    if (sampler)
      return sampler;

    m_sampler = createSampler();
    sampler = m_sampler->handle();

    m_samplerHandle.store(sampler, std::memory_order_release);
    return sampler;
  }


  Rc<DxvkSampler> DxvkUnboundResources::createSampler() {
    DxvkSamplerKey info;
    info.setFilter(VK_FILTER_LINEAR, VK_FILTER_LINEAR, VK_SAMPLER_MIPMAP_MODE_LINEAR);
    info.setLodRange(-256.0f, 256.0f, 0.0f);
    info.setAddressModes(
      VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
      VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
      VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);
    info.setReduction(VK_SAMPLER_REDUCTION_MODE_WEIGHTED_AVERAGE);

    return m_device->createSampler(info);
  }
  
  
  Rc<DxvkBuffer> DxvkUnboundResources::createBuffer() {
    DxvkBufferCreateInfo info;
    info.size       = MaxUniformBufferSize;
    info.usage      = VK_BUFFER_USAGE_TRANSFER_DST_BIT
                    | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT
                    | VK_BUFFER_USAGE_INDEX_BUFFER_BIT
                    | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT
                    | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT
                    | VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT
                    | VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT
                    | VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_BUFFER_BIT_EXT;
    info.stages     = VK_PIPELINE_STAGE_TRANSFER_BIT
                    | m_device->getShaderPipelineStages();
    info.access     = VK_ACCESS_UNIFORM_READ_BIT
                    | VK_ACCESS_SHADER_READ_BIT
                    | VK_ACCESS_SHADER_WRITE_BIT;
    info.debugName  = "Null buffer";
    
    Rc<DxvkBuffer> buffer = m_device->createBuffer(info,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
      VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    std::memset(buffer->mapPtr(0), 0, info.size);
    return buffer;
  }
  
  VkDescriptorImageInfo DxvkUnboundResources::imageDescriptor(VkImageViewType type) {
    if (type == VK_IMAGE_VIEW_TYPE_MAX_ENUM)
      type = VK_IMAGE_VIEW_TYPE_2D;
    std::lock_guard lock(m_mutex);
    auto it = m_dummyImages.find(type);
    if (it == m_dummyImages.end()) {
      DxvkImageCreateInfo info;
      switch (type) {
        case VK_IMAGE_VIEW_TYPE_1D:
        case VK_IMAGE_VIEW_TYPE_1D_ARRAY: info.type = VK_IMAGE_TYPE_1D; break;
        case VK_IMAGE_VIEW_TYPE_2D:
        case VK_IMAGE_VIEW_TYPE_2D_ARRAY: info.type = VK_IMAGE_TYPE_2D; break;
        case VK_IMAGE_VIEW_TYPE_CUBE:
        case VK_IMAGE_VIEW_TYPE_CUBE_ARRAY:
          info.type = VK_IMAGE_TYPE_2D;
          info.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
          break;
        case VK_IMAGE_VIEW_TYPE_3D: info.type = VK_IMAGE_TYPE_3D; break;
        default: throw DxvkError("BMVR dummy sampled image: unsupported view type");
      }
      info.format = VK_FORMAT_R32_SFLOAT;
      info.sampleCount = VK_SAMPLE_COUNT_1_BIT;
      info.extent = {1, 1, 1};
      info.numLayers = (info.flags & VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT) ? 6 : 1;
      info.mipLevels = 1;
      info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
      info.stages = m_device->getShaderPipelineStages() | VK_PIPELINE_STAGE_TRANSFER_BIT;
      info.access = VK_ACCESS_SHADER_READ_BIT;
      info.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
      info.debugName = "BMVR unbound sampled image";
      DummyImage entry;
      entry.image = m_device->createImage(info, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
      DxvkImageViewKey key;
      key.viewType = type;
      key.format = info.format;
      key.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
      key.layout = info.layout;
      key.aspects = VK_IMAGE_ASPECT_COLOR_BIT;
      key.mipCount = 1;
      key.layerCount = info.numLayers;
      // Return zero for every component, including alpha (D3D9 null texture).
      for (unsigned component = 0; component < 4; component++)
        key.packedSwizzle |= uint16_t(VK_COMPONENT_SWIZZLE_ZERO) << (4 * component);
      entry.view = entry.image->createView(key);
      auto ctx = m_device->createContext();
      ctx->beginRecording(m_device->createCommandList());
      ctx->initImage(entry.image, VK_IMAGE_LAYOUT_UNDEFINED);
      auto recorded = ctx->endRecording(nullptr);
      m_device->submitCommandList(recorded.first, nullptr, 0, recorded.second, nullptr);
      it = m_dummyImages.emplace(type, std::move(entry)).first;
    }
    const auto* descriptor = it->second.view->getDescriptor(type);
    if (!descriptor)
      throw DxvkError("BMVR dummy sampled image: descriptor creation failed");
    return descriptor->legacy.image;
  }

}
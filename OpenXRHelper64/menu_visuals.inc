// Included inside Vulkan helper. A tiny static atlas is uploaded once on the
// first menu. Later frames only append compositor quads: no scene rerender,
// per-frame texture upload, new fence, or additional queue wait.
VulkanEyeSwapchain m_MenuVisualSprite{};
bool m_MenuVisualReady = false, m_MenuVisualAttempted = false;

bool EnsureMenuVisualSprite()
{
    if (m_MenuVisualReady)
        return true;
    if (m_MenuVisualAttempted || !m_UseShaderBlit)
        return false;
    m_MenuVisualAttempted = true;
    auto clearRect = reinterpret_cast<PFN_vkCmdClearAttachments>(
        m_Vk.vkGetDeviceProcAddr(m_VkDevice, "vkCmdClearAttachments"));
    if (!clearRect)
        return false;
    auto &image = m_MenuVisualSprite;
    image.width = image.height = 64;
    image.format = m_SelectedSwapchainFormat;
    XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
    ci.format = image.format;
    ci.sampleCount = ci.faceCount = ci.arraySize = ci.mipCount = 1;
    ci.width = ci.height = 64;
    if (XR_FAILED(m_Xr.xrCreateSwapchain(m_Session, &ci, &image.handle)))
        return false;
    uint32_t count = 0;
    if (XR_FAILED(m_Xr.xrEnumerateSwapchainImages(image.handle, 0, &count, nullptr)) || !count)
        return false;
    image.images.assign(count, XrSwapchainImageVulkanKHR{XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR});
    if (XR_FAILED(m_Xr.xrEnumerateSwapchainImages(
            image.handle, count, &count,
            reinterpret_cast<XrSwapchainImageBaseHeader *>(image.images.data()))))
        return false;
    image.images.resize(count);
    image.layouts.assign(count, VK_IMAGE_LAYOUT_UNDEFINED);
    if (!CreateSwapchainRenderTargets(image))
        return false;
    uint32_t index = 0;
    XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    if (XR_FAILED(m_Xr.xrAcquireSwapchainImage(image.handle, &acquire, &index)))
        return false;
    XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wait.timeout = XR_INFINITE_DURATION;
    if (XR_FAILED(m_Xr.xrWaitSwapchainImage(image.handle, &wait)))
        return false;
    XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    auto failed = [&]() {
        m_Xr.xrReleaseSwapchainImage(image.handle, &release);
        return false;
    };
    m_CommandBuffer = m_CommandBuffers[2];
    if (m_Vk.vkResetCommandBuffer(m_CommandBuffer, 0) != VK_SUCCESS)
        return failed();
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (m_Vk.vkBeginCommandBuffer(m_CommandBuffer, &begin) != VK_SUCCESS)
        return failed();
    CmdTransitionImage(image.images[index].image, VK_IMAGE_LAYOUT_UNDEFINED,
                       VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                       VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    VkClearValue transparent{};
    VkRenderPassBeginInfo pass{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    pass.renderPass = m_BlitRenderPass;
    pass.framebuffer = image.framebuffers[index];
    pass.renderArea.extent = {64, 64};
    pass.clearValueCount = 1;
    pass.pClearValues = &transparent;
    m_Vk.vkCmdBeginRenderPass(m_CommandBuffer, &pass, VK_SUBPASS_CONTENTS_INLINE);
    auto rect = [&](int x, int y, uint32_t w, uint32_t h, float r, float g, float b) {
        VkClearAttachment a{};
        a.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        a.clearValue.color.float32[0] = r;
        a.clearValue.color.float32[1] = g;
        a.clearValue.color.float32[2] = b;
        a.clearValue.color.float32[3] = 1;
        VkClearRect box{};
        box.rect.offset = {x, y};
        box.rect.extent = {w, h};
        box.layerCount = 1;
        clearRect(m_CommandBuffer, 1, &a, 1, &box);
    };
    // Simple orange disk for each physical hand; no controller model needed.
    for (int y = 2; y < 62; ++y)
    {
        const float dy = (float(y) + .5f - 32.f) / 30.f;
        const int half = int(22.f * std::sqrt(std::max(0.f, 1.f - dy * dy)));
        if (half > 0)
            rect(24 - half, y, uint32_t(2 * half), 1, 1.f, .28f, .025f);
    }
    rect(56, 0, 8, 64, .95f, .65f, .20f); // solid colour subrect for the beam
    m_Vk.vkCmdEndRenderPass(m_CommandBuffer);
    if (m_Vk.vkEndCommandBuffer(m_CommandBuffer) != VK_SUCCESS)
        return failed();
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &m_CommandBuffer;
    if (m_Vk.vkQueueSubmit(m_GraphicsQueue, 1, &submit, VK_NULL_HANDLE) != VK_SUCCESS)
        return failed();
    if (m_Vk.vkQueueWaitIdle(m_GraphicsQueue) != VK_SUCCESS)
        return failed();
    image.layouts[index] = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    if (XR_FAILED(m_Xr.xrReleaseSwapchainImage(image.handle, &release)))
        return false;
    m_MenuVisualReady = true;
    m_Log.Print(
        "[BMVR menu v58] static controller/beam atlas ready; compositor quads only after initialization");
    return true;
}

static XrVector3f MenuSub(XrVector3f a, XrVector3f b)
{
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}
static XrVector3f MenuCross(XrVector3f a, XrVector3f b)
{
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
static bool MenuNormalise(XrVector3f &v)
{
    float n = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    if (!std::isfinite(n) || n < 1e-5f)
        return false;
    v = {v.x / n, v.y / n, v.z / n};
    return true;
}
static XrQuaternionf MenuBasisQuaternion(XrVector3f x, XrVector3f y, XrVector3f z)
{
    XrQuaternionf q{};
    const float trace = x.x + y.y + z.z;
    if (trace > 0)
    {
        float s = 2 * std::sqrt(trace + 1);
        q = {(y.z - z.y) / s, (z.x - x.z) / s, (x.y - y.x) / s, s / 4};
    }
    else if (x.x > y.y && x.x > z.z)
    {
        float s = 2 * std::sqrt(1 + x.x - y.y - z.z);
        q = {s / 4, (y.x + x.y) / s, (z.x + x.z) / s, (y.z - z.y) / s};
    }
    else if (y.y > z.z)
    {
        float s = 2 * std::sqrt(1 + y.y - x.x - z.z);
        q = {(y.x + x.y) / s, s / 4, (z.y + y.z) / s, (z.x - x.z) / s};
    }
    else
    {
        float s = 2 * std::sqrt(1 + z.z - x.x - y.y);
        q = {(z.x + x.z) / s, (z.y + y.z) / s, s / 4, (x.y - y.x) / s};
    }
    return q;
}

void AppendMenuVisuals(std::array<XrCompositionLayerQuad, kMaxOpenXrOverlayLayers> &layers, uint32_t &count,
                       const std::vector<XrView> &views)
{
    L4D2VROpenXrMenuPointerDesc pointer{};
    L4D2VROpenXrInputStateDesc input{};
    if (!m_Bridge.ReadMenuVisualState(pointer, input) || !pointer.active || !input.valid || !count)
        return;
    // The game sends actual smoothed cursor UVs, including sensitivity gain.
    // Use the final submitted menu quad so beam and cursor share its transform.
    const auto panel = layers[count - 1];
    if (!EnsureMenuVisualSprite())
        return;
    const auto headQ = views[0].pose.orientation;
    const XrVector3f head = {(views[0].pose.position.x + views[1].pose.position.x) * .5f,
                             (views[0].pose.position.y + views[1].pose.position.y) * .5f,
                             (views[0].pose.position.z + views[1].pose.position.z) * .5f};
    // OpenXR guarantees at least 16 composition layers; keep one for scene.
    const uint32_t limit = std::min<uint32_t>(uint32_t(layers.size()), 15u);
    auto append = [&](XrPosef pose, float width, float height, XrRect2Di crop) {
        if (count >= limit)
            return;
        auto &q = layers[count++];
        q = XrCompositionLayerQuad{XR_TYPE_COMPOSITION_LAYER_QUAD};
        q.space = m_AppSpace;
        q.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
        q.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT |
                       XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT;
        q.pose = pose;
        q.size = {width, height};
        q.subImage.swapchain = m_MenuVisualSprite.handle;
        q.subImage.imageRect = crop;
    };
    for (uint32_t h = 0; h < 2; ++h)
    {
        const auto &grip = input.controllerPoses[h];
        if (!grip.valid || !grip.active)
            continue;
        XrPosef pose{headQ, {grip.position[0], grip.position[1], grip.position[2]}};
        if (!std::isfinite(pose.position.x) || !std::isfinite(pose.position.y) ||
            !std::isfinite(pose.position.z))
            continue;
        append(pose, .065f, .065f, {{0, 0}, {48, 64}});
    }
    const auto &grip = input.controllerPoses[L4D2VR_OPENXR_HAND_RIGHT];
    const auto &aim = input.controllerAimPoses[L4D2VR_OPENXR_HAND_RIGHT];
    const auto &hand = (aim.valid && aim.active) ? aim : grip;
    if (!hand.valid || !hand.active)
        return;
    XrVector3f start = {hand.position[0], hand.position[1], hand.position[2]}, end{};
    if (pointer.rayValid)
        start = {pointer.rayOrigin[0], pointer.rayOrigin[1], pointer.rayOrigin[2]};
    if (pointer.hit)
    {
        const XrVector3f local = {(std::clamp(pointer.u, 0.f, 1.f) - .5f) * panel.size.width,
                                  (.5f - std::clamp(pointer.v, 0.f, 1.f)) * panel.size.height, .002f};
        const auto offset = RotateOpenXrVector(panel.pose.orientation, local);
        end = {panel.pose.position.x + offset.x, panel.pose.position.y + offset.y,
               panel.pose.position.z + offset.z};
    }
    else
    {
        const auto dir =
            pointer.rayValid
                ? XrVector3f{pointer.rayDirection[0], pointer.rayDirection[1], pointer.rayDirection[2]}
                : RotateOpenXrVector(NormalizeOpenXrQuaternion(hand.orientation), {0, 0, -1});
        end = {start.x + dir.x * .5f, start.y + dir.y * .5f, start.z + dir.z * .5f};
    }
    auto axis = MenuSub(end, start);
    const float length = std::sqrt(axis.x * axis.x + axis.y * axis.y + axis.z * axis.z);
    if (!std::isfinite(length) || length < .015f || length > 4.f || !MenuNormalise(axis))
        return;
    XrVector3f center = {(start.x + end.x) * .5f, (start.y + end.y) * .5f, (start.z + end.z) * .5f};
    auto right = MenuCross(axis, MenuSub(head, center));
    if (!MenuNormalise(right))
        return;
    const auto normal = MenuCross(right, axis);
    append({MenuBasisQuaternion(right, axis, normal), center}, .002f, length, {{56, 0}, {8, 64}});
}

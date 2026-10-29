#include <render/VideoFrameRenderer.h>

#include <shader/yuv_to_rgb.comp.h>

#include <vkutils/util.h>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_vulkan.h>
#include <libavutil/pixdesc.h>
}

#include <SDL3/SDL.h>

#include <cmath>
#include <cstring>


namespace {

// Push constants of yuv_to_rgb.comp
struct ConversionParameters
{
    float row0[4];
    float row1[4];
    float row2[4];
    float range[4];
    float codeScale;
    float padding[3];
};


// Layout of the planes of the formats the shader converts.
struct PlaneFormat
{
    VkFormat luma;
    VkFormat chroma;
    int bits;               // Of the code values
    float codeScale;        // Sample to code value
};


bool planeFormat(AVPixelFormat format, PlaneFormat& planes)
{
    switch (format) {
        case AV_PIX_FMT_NV12:
            planes = { VK_FORMAT_R8_UNORM, VK_FORMAT_R8G8_UNORM, 8, 255.f };
            return true;
        case AV_PIX_FMT_P010:
            // 10 bits in the high bits of 16
            planes = { VK_FORMAT_R16_UNORM, VK_FORMAT_R16G16_UNORM, 10, 65535.f / 64.f };
            return true;
        case AV_PIX_FMT_P012:
            planes = { VK_FORMAT_R16_UNORM, VK_FORMAT_R16G16_UNORM, 12, 65535.f / 16.f };
            return true;
        case AV_PIX_FMT_P016:
            planes = { VK_FORMAT_R16_UNORM, VK_FORMAT_R16G16_UNORM, 16, 65535.f };
            return true;
        default:
            return false;
    }
}


ConversionParameters conversionParameters(const AVFrame& frame, const PlaneFormat& planes)
{
    // Luma coefficients of the color space. Untagged streams: BT.709 for HD,
    // BT.601 for SD, as players do.
    float kr = 0.2126f;
    float kb = 0.0722f;

    switch (frame.colorspace) {
        case AVCOL_SPC_BT470BG:
        case AVCOL_SPC_SMPTE170M:
            kr = 0.299f;
            kb = 0.114f;
            break;
        case AVCOL_SPC_BT2020_NCL:
        case AVCOL_SPC_BT2020_CL:
            kr = 0.2627f;
            kb = 0.0593f;
            break;
        case AVCOL_SPC_UNSPECIFIED:
            if (frame.height < 720) {
                kr = 0.299f;
                kb = 0.114f;
            }
            break;
        default:
            break;
    }

    const float kg = 1.f - kr - kb;

    ConversionParameters parameters = {};

    parameters.row0[0] = 1.f;
    parameters.row0[2] = 2.f * (1.f - kr);

    parameters.row1[0] = 1.f;
    parameters.row1[1] = -2.f * kb * (1.f - kb) / kg;
    parameters.row1[2] = -2.f * kr * (1.f - kr) / kg;

    parameters.row2[0] = 1.f;
    parameters.row2[1] = 2.f * (1.f - kb);

    // Code values range.
    const float scale = std::ldexp(1.f, planes.bits - 8);
    const float maxCode = std::ldexp(1.f, planes.bits) - 1.f;

    if (frame.color_range == AVCOL_RANGE_JPEG) {
        parameters.range[0] = 0.f;
        parameters.range[1] = maxCode;
        parameters.range[2] = std::ldexp(1.f, planes.bits - 1);
        parameters.range[3] = maxCode;
    }
    else {
        parameters.range[0] = 16.f * scale;
        parameters.range[1] = 219.f * scale;
        parameters.range[2] = 128.f * scale;
        parameters.range[3] = 224.f * scale;
    }

    parameters.codeScale = planes.codeScale;

    return parameters;
}

} // namespace


VideoFrameRenderer::VideoFrameRenderer(const GPU::Device& device, const GPU::PipelineCache& pipelineCache)
    : _device(device)
{
    createPipeline(pipelineCache);
}


VideoFrameRenderer::~VideoFrameRenderer()
{
    VkDevice device = _device.device();

    beginFrame();

    _image.reset();
    _staging.reset();
    _shownFrame.reset();

    vkDestroyDescriptorPool(device, _descriptorPool, nullptr);
    vkDestroyPipeline(device, _pipeline, nullptr);
    vkDestroyPipelineLayout(device, _pipelineLayout, nullptr);
    vkDestroyDescriptorSetLayout(device, _descriptorSetLayout, nullptr);
    vkDestroySampler(device, _sampler, nullptr);
}


void VideoFrameRenderer::createPipeline(const GPU::PipelineCache& pipelineCache)
{
    VkDevice device = _device.device();

    // Bilinear for the chroma planes, which are smaller than the picture.
    VkSamplerCreateInfo samplerInfo = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.maxLod = 0.f;

    VK_THROW_IF_FAILED(vkCreateSampler(device, &samplerInfo, nullptr, &_sampler));

    const VkDescriptorSetLayoutBinding bindings[] = {
        { 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
        { 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
        { 2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,          1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
    };

    VkDescriptorSetLayoutCreateInfo layoutInfo = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    layoutInfo.bindingCount = C_ARRAY_SIZE(bindings);
    layoutInfo.pBindings = bindings;

    VK_THROW_IF_FAILED(vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &_descriptorSetLayout));

    const VkPushConstantRange pushConstants = { VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(ConversionParameters) };

    VkPipelineLayoutCreateInfo pipelineLayoutInfo = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    pipelineLayoutInfo.setLayoutCount = 1;
    pipelineLayoutInfo.pSetLayouts = &_descriptorSetLayout;
    pipelineLayoutInfo.pushConstantRangeCount = 1;
    pipelineLayoutInfo.pPushConstantRanges = &pushConstants;

    VK_THROW_IF_FAILED(vkCreatePipelineLayout(device, &pipelineLayoutInfo, nullptr, &_pipelineLayout));

    VkShaderModuleCreateInfo moduleInfo = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    moduleInfo.codeSize = yuv_to_rgb_comp_size;
    moduleInfo.pCode = yuv_to_rgb_comp;

    VkShaderModule module = VK_NULL_HANDLE;
    VK_THROW_IF_FAILED(vkCreateShaderModule(device, &moduleInfo, nullptr, &module));

    VkComputePipelineCreateInfo pipelineInfo = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
    pipelineInfo.stage = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
    pipelineInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pipelineInfo.stage.module = module;
    pipelineInfo.stage.pName = "main";
    pipelineInfo.layout = _pipelineLayout;

    const VkResult result = vkCreateComputePipelines(device, pipelineCache.pipelineCache(), 1, &pipelineInfo, nullptr, &_pipeline);
    vkDestroyShaderModule(device, module, nullptr);
    VK_THROW_IF_FAILED(result);

    // One set per frame, recycled by beginFrame().
    const VkDescriptorPoolSize poolSizes[] = {
        { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4 },
        { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2 },
    };

    VkDescriptorPoolCreateInfo poolInfo = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    poolInfo.maxSets = 2;
    poolInfo.poolSizeCount = C_ARRAY_SIZE(poolSizes);
    poolInfo.pPoolSizes = poolSizes;

    VK_THROW_IF_FAILED(vkCreateDescriptorPool(device, &poolInfo, nullptr, &_descriptorPool));
}


void VideoFrameRenderer::beginFrame()
{
    VkDevice device = _device.device();

    for (VkImageView view : _viewsInFlight) {
        vkDestroyImageView(device, view, nullptr);
    }

    _viewsInFlight.clear();
    _framesInFlight.clear();

    vkResetDescriptorPool(device, _descriptorPool, 0);
}


void VideoFrameRenderer::ensureImage(uint32_t width, uint32_t height)
{
    if (_image && _image->width() == width && _image->height() == height) {
        return;
    }

    // The previous frame is done with the image, see beginFrame().
    _image = std::make_unique<GPU::Image>(
        "Video frame",
        _device,
        VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        VK_FORMAT_R8G8B8A8_UNORM,
        width, height);
}


bool VideoFrameRenderer::cmdRender(VkCommandBuffer commandBuffer, const std::shared_ptr<media::VideoFrame>& frame, Sync& sync)
{
    if (!frame || frame == _shownFrame) {
        return true;
    }

    if (frame->width == 0 || frame->height == 0) {
        return false;
    }

    // The decoder must not reuse the frame before the GPU is done with it.
    _framesInFlight.push_back(frame);

    if (frame->hwFrame) {
        if (!cmdConvertHardwareFrame(commandBuffer, *frame, sync)) {
            return false;
        }
    }
    else if (!frame->rgba.empty()) {
        cmdUploadSoftwareFrame(commandBuffer, *frame);
    }
    else {
        return false;
    }

    _shownFrame = frame;

    return true;
}


bool VideoFrameRenderer::cmdConvertHardwareFrame(VkCommandBuffer commandBuffer, const media::VideoFrame& frame, Sync& sync)
{
    const AVFrame* avFrame = frame.hwFrame;
    AVHWFramesContext* framesContext = (AVHWFramesContext*)avFrame->hw_frames_ctx->data;
    AVVulkanFramesContext* vulkanFrames = (AVVulkanFramesContext*)framesContext->hwctx;
    AVVkFrame* vkFrame = (AVVkFrame*)avFrame->data[0];

    PlaneFormat planes;

    if (!planeFormat(framesContext->sw_format, planes)) {
        if (!_reportedUnsupported) {
            _reportedUnsupported = true;
            SDL_Log("Unsupported decoded video format: %s", av_get_pix_fmt_name(framesContext->sw_format));
        }

        return false;
    }

    ensureImage(frame.width, frame.height);

    VkDevice device = _device.device();

    // A single multi-plane image, or one image per plane.
    const bool separatePlanes = vkFrame->img[1] != VK_NULL_HANDLE;
    const int imageCount = separatePlanes ? 2 : 1;
    const bool disjoint = (vulkanFrames->img_flags & VK_IMAGE_CREATE_DISJOINT_BIT) != 0;

    vulkanFrames->lock_frame(framesContext, vkFrame);

    // Plane views, sampled only: the images are created by FFmpeg with
    // mutable formats and extended usage for that.
    VkImageView views[2] = {};

    for (int plane = 0; plane < 2; plane++) {
        const VkImageViewUsageCreateInfo usageInfo = {
            VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO, nullptr, VK_IMAGE_USAGE_SAMPLED_BIT
        };

        VkImageViewCreateInfo viewInfo = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        viewInfo.pNext = &usageInfo;
        viewInfo.image = separatePlanes ? vkFrame->img[plane] : vkFrame->img[0];
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = (plane == 0) ? planes.luma : planes.chroma;
        viewInfo.subresourceRange = {
            separatePlanes ? (VkImageAspectFlags)VK_IMAGE_ASPECT_COLOR_BIT
                           : (VkImageAspectFlags)(plane == 0 ? VK_IMAGE_ASPECT_PLANE_0_BIT : VK_IMAGE_ASPECT_PLANE_1_BIT),
            0, 1, 0, 1
        };

        if (vkCreateImageView(device, &viewInfo, nullptr, &views[plane]) != VK_SUCCESS) {
            vulkanFrames->unlock_frame(framesContext, vkFrame);

            for (VkImageView view : views) {
                if (view) vkDestroyImageView(device, view, nullptr);
            }

            return false;
        }

        _viewsInFlight.push_back(views[plane]);
    }

    // Wait for the decoder, take the images to the sampling layout, and
    // tell the decoder when we are done with them.
    std::vector<VkImageMemoryBarrier> barriers;

    for (int i = 0; i < imageCount; i++) {
        sync.waits.push_back({ vkFrame->sem[i], vkFrame->sem_value[i] });
        sync.signals.push_back({ vkFrame->sem[i], vkFrame->sem_value[i] + 1 });

        VkImageMemoryBarrier barrier = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        barrier.srcAccessMask = 0;          // Made available by the semaphore
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        barrier.oldLayout = vkFrame->layout[i];
        barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;  // Concurrent sharing
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = vkFrame->img[i];
        barrier.subresourceRange = {
            (!separatePlanes && disjoint)
                ? (VkImageAspectFlags)(VK_IMAGE_ASPECT_PLANE_0_BIT | VK_IMAGE_ASPECT_PLANE_1_BIT)
                : (VkImageAspectFlags)VK_IMAGE_ASPECT_COLOR_BIT,
            0, 1, 0, 1
        };

        barriers.push_back(barrier);

        vkFrame->sem_value[i]++;
        vkFrame->layout[i] = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        vkFrame->access[i] = (VkAccessFlagBits)VK_ACCESS_SHADER_READ_BIT;
    }

    vulkanFrames->unlock_frame(framesContext, vkFrame);

    // The waits happen at the compute shader stage, which the barriers chain
    // with.
    vkCmdPipelineBarrier(
        commandBuffer,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0,
        0, nullptr,
        0, nullptr,
        (uint32_t)barriers.size(), barriers.data());

    _image->cmdBarrierTransition(commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_IMAGE_LAYOUT_GENERAL);

    // Descriptors
    VkDescriptorSetAllocateInfo allocateInfo = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    allocateInfo.descriptorPool = _descriptorPool;
    allocateInfo.descriptorSetCount = 1;
    allocateInfo.pSetLayouts = &_descriptorSetLayout;

    VkDescriptorSet set = VK_NULL_HANDLE;
    VK_THROW_IF_FAILED(vkAllocateDescriptorSets(device, &allocateInfo, &set));

    const VkDescriptorImageInfo imageInfos[] = {
        { _sampler, views[0], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
        { _sampler, views[1], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
        { VK_NULL_HANDLE, _image->imageView(), VK_IMAGE_LAYOUT_GENERAL },
    };

    VkWriteDescriptorSet writes[3] = {};

    for (uint32_t i = 0; i < 3; i++) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = set;
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = (i < 2) ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[i].pImageInfo = &imageInfos[i];
    }

    vkUpdateDescriptorSets(device, 3, writes, 0, nullptr);

    // Conversion
    const ConversionParameters parameters = conversionParameters(*avFrame, planes);

    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, _pipeline);
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, _pipelineLayout, 0, 1, &set, 0, nullptr);
    vkCmdPushConstants(commandBuffer, _pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(parameters), &parameters);
    vkCmdDispatch(commandBuffer, (frame.width + 15) / 16, (frame.height + 15) / 16, 1);

    _image->cmdBarrierTransition(commandBuffer, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    return true;
}


void VideoFrameRenderer::cmdUploadSoftwareFrame(VkCommandBuffer commandBuffer, const media::VideoFrame& frame)
{
    ensureImage(frame.width, frame.height);

    const VkDeviceSize size = (VkDeviceSize)frame.rgba.size();

    if (!_staging || _staging->size() < size) {
        _staging = std::make_unique<GPU::Buffer>(
            "Video frame upload",
            _device,
            size,
            VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

        _staging->bind();
    }

    // The previous upload is done, see beginFrame().
    std::memcpy(_staging->mappedMemory(), frame.rgba.data(), (size_t)size);

    _image->cmdBarrierTransition(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

    VkBufferImageCopy region = {};
    region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    region.imageExtent = { frame.width, frame.height, 1 };

    vkCmdCopyBufferToImage(commandBuffer, _staging->buffer(), _image->image(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    _image->cmdBarrierTransition(commandBuffer, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

#include <render/Nv12Converter.h>

#include <shader/rgb_to_nv12.comp.h>

#include <vkutils/util.h>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_vulkan.h>
}


namespace {

// Push constants of rgb_to_nv12.comp
struct ConversionParameters
{
    float row0[4];
    float row1[4];
    float row2[4];
    float range[4];
};


ConversionParameters bt709LimitedRange()
{
    const float kr = 0.2126f;
    const float kb = 0.0722f;
    const float kg = 1.f - kr - kb;

    ConversionParameters parameters = {};

    // Y'
    parameters.row0[0] = kr;
    parameters.row0[1] = kg;
    parameters.row0[2] = kb;

    // Cb = (B' - Y') / (2 (1 - kb))
    parameters.row1[0] = -kr / (2.f * (1.f - kb));
    parameters.row1[1] = -kg / (2.f * (1.f - kb));
    parameters.row1[2] = 0.5f;

    // Cr = (R' - Y') / (2 (1 - kr))
    parameters.row2[0] = 0.5f;
    parameters.row2[1] = -kg / (2.f * (1.f - kr));
    parameters.row2[2] = -kb / (2.f * (1.f - kr));

    // 8 bits limited range: Y' 16..235, Cb Cr 16..240 around 128.
    parameters.range[0] = 16.f / 255.f;
    parameters.range[1] = 219.f / 255.f;
    parameters.range[2] = 128.f / 255.f;
    parameters.range[3] = 224.f / 255.f;

    return parameters;
}

} // namespace


Nv12Converter::Nv12Converter(const GPU::Device& device, const GPU::PipelineCache& pipelineCache)
    : _device(device)
{
    VkDevice vkDevice = _device.device();

    VkSamplerCreateInfo samplerInfo = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    samplerInfo.magFilter = VK_FILTER_NEAREST;
    samplerInfo.minFilter = VK_FILTER_NEAREST;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    VK_THROW_IF_FAILED(vkCreateSampler(vkDevice, &samplerInfo, nullptr, &_sampler));

    const VkDescriptorSetLayoutBinding bindings[] = {
        { 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
        { 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,          1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
        { 2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,          1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
    };

    VkDescriptorSetLayoutCreateInfo layoutInfo = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    layoutInfo.bindingCount = C_ARRAY_SIZE(bindings);
    layoutInfo.pBindings = bindings;
    VK_THROW_IF_FAILED(vkCreateDescriptorSetLayout(vkDevice, &layoutInfo, nullptr, &_descriptorSetLayout));

    const VkPushConstantRange pushConstants = { VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(ConversionParameters) };

    VkPipelineLayoutCreateInfo pipelineLayoutInfo = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    pipelineLayoutInfo.setLayoutCount = 1;
    pipelineLayoutInfo.pSetLayouts = &_descriptorSetLayout;
    pipelineLayoutInfo.pushConstantRangeCount = 1;
    pipelineLayoutInfo.pPushConstantRanges = &pushConstants;
    VK_THROW_IF_FAILED(vkCreatePipelineLayout(vkDevice, &pipelineLayoutInfo, nullptr, &_pipelineLayout));

    VkShaderModuleCreateInfo moduleInfo = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    moduleInfo.codeSize = rgb_to_nv12_comp_size;
    moduleInfo.pCode = rgb_to_nv12_comp;

    VkShaderModule module = VK_NULL_HANDLE;
    VK_THROW_IF_FAILED(vkCreateShaderModule(vkDevice, &moduleInfo, nullptr, &module));

    VkComputePipelineCreateInfo pipelineInfo = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
    pipelineInfo.stage = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
    pipelineInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pipelineInfo.stage.module = module;
    pipelineInfo.stage.pName = "main";
    pipelineInfo.layout = _pipelineLayout;

    const VkResult result = vkCreateComputePipelines(vkDevice, pipelineCache.pipelineCache(), 1, &pipelineInfo, nullptr, &_pipeline);
    vkDestroyShaderModule(vkDevice, module, nullptr);
    VK_THROW_IF_FAILED(result);

    const VkDescriptorPoolSize poolSizes[] = {
        { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2 },
        { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 4 },
    };

    VkDescriptorPoolCreateInfo poolInfo = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    poolInfo.maxSets = 2;
    poolInfo.poolSizeCount = C_ARRAY_SIZE(poolSizes);
    poolInfo.pPoolSizes = poolSizes;
    VK_THROW_IF_FAILED(vkCreateDescriptorPool(vkDevice, &poolInfo, nullptr, &_descriptorPool));
}


Nv12Converter::~Nv12Converter()
{
    VkDevice device = _device.device();

    beginFrame();

    vkDestroyDescriptorPool(device, _descriptorPool, nullptr);
    vkDestroyPipeline(device, _pipeline, nullptr);
    vkDestroyPipelineLayout(device, _pipelineLayout, nullptr);
    vkDestroyDescriptorSetLayout(device, _descriptorSetLayout, nullptr);
    vkDestroySampler(device, _sampler, nullptr);
}


void Nv12Converter::beginFrame()
{
    for (VkImageView view : _viewsInFlight) {
        vkDestroyImageView(_device.device(), view, nullptr);
    }

    _viewsInFlight.clear();
    vkResetDescriptorPool(_device.device(), _descriptorPool, 0);
}


bool Nv12Converter::cmdConvert(VkCommandBuffer commandBuffer, const GPU::Image& source, AVFrame* frame, VideoFrameRenderer::Sync& sync)
{
    VkDevice device = _device.device();

    AVHWFramesContext* framesContext = (AVHWFramesContext*)frame->hw_frames_ctx->data;
    AVVulkanFramesContext* vulkanFrames = (AVVulkanFramesContext*)framesContext->hwctx;
    AVVkFrame* vkFrame = (AVVkFrame*)frame->data[0];

    if (framesContext->sw_format != AV_PIX_FMT_NV12) {
        return false;
    }

    const bool separatePlanes = vkFrame->img[1] != VK_NULL_HANDLE;
    const int imageCount = separatePlanes ? 2 : 1;
    const bool disjoint = (vulkanFrames->img_flags & VK_IMAGE_CREATE_DISJOINT_BIT) != 0;

    vulkanFrames->lock_frame(framesContext, vkFrame);

    // Plane views written by the shader.
    VkImageView views[2] = {};

    for (int plane = 0; plane < 2; plane++) {
        const VkImageViewUsageCreateInfo usageInfo = {
            VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO, nullptr, VK_IMAGE_USAGE_STORAGE_BIT
        };

        VkImageViewCreateInfo viewInfo = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        viewInfo.pNext = &usageInfo;
        viewInfo.image = separatePlanes ? vkFrame->img[plane] : vkFrame->img[0];
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = (plane == 0) ? VK_FORMAT_R8_UNORM : VK_FORMAT_R8G8_UNORM;
        viewInfo.subresourceRange = {
            separatePlanes ? (VkImageAspectFlags)VK_IMAGE_ASPECT_COLOR_BIT
                           : (VkImageAspectFlags)(plane == 0 ? VK_IMAGE_ASPECT_PLANE_0_BIT : VK_IMAGE_ASPECT_PLANE_1_BIT),
            0, 1, 0, 1
        };

        if (vkCreateImageView(device, &viewInfo, nullptr, &views[plane]) != VK_SUCCESS) {
            vulkanFrames->unlock_frame(framesContext, vkFrame);
            return false;
        }

        _viewsInFlight.push_back(views[plane]);
    }

    // Wait for the encoder to be done with the image (a frame of its pool),
    // write it, and hand it back.
    std::vector<VkImageMemoryBarrier> barriers;

    for (int i = 0; i < imageCount; i++) {
        sync.waits.push_back({ vkFrame->sem[i], vkFrame->sem_value[i] });
        sync.signals.push_back({ vkFrame->sem[i], vkFrame->sem_value[i] + 1 });

        VkImageMemoryBarrier barrier = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;     // Overwritten
        barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
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
        vkFrame->layout[i] = VK_IMAGE_LAYOUT_GENERAL;
        vkFrame->access[i] = (VkAccessFlagBits)VK_ACCESS_SHADER_WRITE_BIT;
    }

    vulkanFrames->unlock_frame(framesContext, vkFrame);

    vkCmdPipelineBarrier(
        commandBuffer,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0,
        0, nullptr,
        0, nullptr,
        (uint32_t)barriers.size(), barriers.data());

    VkDescriptorSetAllocateInfo allocateInfo = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    allocateInfo.descriptorPool = _descriptorPool;
    allocateInfo.descriptorSetCount = 1;
    allocateInfo.pSetLayouts = &_descriptorSetLayout;

    VkDescriptorSet set = VK_NULL_HANDLE;
    VK_THROW_IF_FAILED(vkAllocateDescriptorSets(device, &allocateInfo, &set));

    const VkDescriptorImageInfo imageInfos[] = {
        { _sampler, source.imageView(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
        { VK_NULL_HANDLE, views[0], VK_IMAGE_LAYOUT_GENERAL },
        { VK_NULL_HANDLE, views[1], VK_IMAGE_LAYOUT_GENERAL },
    };

    VkWriteDescriptorSet writes[3] = {};

    for (uint32_t i = 0; i < 3; i++) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = set;
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = (i == 0) ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[i].pImageInfo = &imageInfos[i];
    }

    vkUpdateDescriptorSets(device, 3, writes, 0, nullptr);

    const ConversionParameters parameters = bt709LimitedRange();

    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, _pipeline);
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, _pipelineLayout, 0, 1, &set, 0, nullptr);
    vkCmdPushConstants(commandBuffer, _pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(parameters), &parameters);

    // One invocation per 2 x 2 block.
    const uint32_t blocksX = (source.width() + 1) / 2;
    const uint32_t blocksY = (source.height() + 1) / 2;
    vkCmdDispatch(commandBuffer, (blocksX + 15) / 16, (blocksY + 15) / 16, 1);

    // The encoder reads the planes after the semaphore signal: make the
    // writes available.
    VkMemoryBarrier memoryBarrier = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
    memoryBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    memoryBarrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;

    vkCmdPipelineBarrier(
        commandBuffer,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
        0,
        1, &memoryBarrier,
        0, nullptr,
        0, nullptr);

    return true;
}

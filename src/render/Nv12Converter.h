#pragma once

#include <render/VideoFrameRenderer.h>

#include <vkutils/Device.h>
#include <vkutils/Image.h>
#include <vkutils/PipelineCache.h>

#include <vector>

struct AVFrame;


// Writes a composited frame into an NV12 frame of an FFmpeg Vulkan frames
// context (the input of the Vulkan Video encoders), by a compute shader: the
// picture never leaves the GPU. BT.709, limited range.
//
// One frame in flight: beginFrame() once the previous submission completed.
class Nv12Converter
{
public:
    Nv12Converter(const GPU::Device& device, const GPU::PipelineCache& pipelineCache);
    ~Nv12Converter();

    Nv12Converter(const Nv12Converter&) = delete;
    Nv12Converter& operator=(const Nv12Converter&) = delete;

    void beginFrame();

    // source: in the shader read only layout. frame: AV_PIX_FMT_VULKAN, NV12,
    // of the size of source. Returns false when the frame cannot be written.
    bool cmdConvert(VkCommandBuffer commandBuffer, const GPU::Image& source, AVFrame* frame, VideoFrameRenderer::Sync& sync);

private:
    const GPU::Device& _device;

    VkSampler _sampler = VK_NULL_HANDLE;
    VkDescriptorSetLayout _descriptorSetLayout = VK_NULL_HANDLE;
    VkPipelineLayout _pipelineLayout = VK_NULL_HANDLE;
    VkPipeline _pipeline = VK_NULL_HANDLE;
    VkDescriptorPool _descriptorPool = VK_NULL_HANDLE;

    std::vector<VkImageView> _viewsInFlight;
};

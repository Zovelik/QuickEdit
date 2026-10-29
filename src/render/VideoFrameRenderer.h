#pragma once

#include <media/VideoReader.h>

#include <vkutils/Buffer.h>
#include <vkutils/Device.h>
#include <vkutils/Image.h>
#include <vkutils/PipelineCache.h>

#include <memory>
#include <vector>


// Turns decoded video frames into an RGBA image, sampled by the compositor.
//
// GPU frames (Vulkan Video, owned by FFmpeg) are converted from YCbCr by a
// compute shader reading the planes directly: no copy, no CPU. CPU frames
// (already RGBA) are uploaded through a staging buffer.
//
// One frame in flight: beginFrame() must be called once the previous
// submission completed, before cmdRender().
class VideoFrameRenderer
{
public:
    // Semaphore operations the submission of the recorded commands must
    // include, to synchronize with the decoder.
    struct SemaphoreOp
    {
        VkSemaphore semaphore;
        uint64_t value;
    };

    struct Sync
    {
        std::vector<SemaphoreOp> waits;     // At the compute shader stage
        std::vector<SemaphoreOp> signals;
    };

    VideoFrameRenderer(const GPU::Device& device, const GPU::PipelineCache& pipelineCache);
    ~VideoFrameRenderer();

    VideoFrameRenderer(const VideoFrameRenderer&) = delete;
    VideoFrameRenderer& operator=(const VideoFrameRenderer&) = delete;

    // Releases the resources of the previous frame.
    void beginFrame();

    // Records the commands that bring frame into the image. Does nothing
    // when the frame is the one already shown. Returns false when the frame
    // format is not supported; the image then keeps its previous content.
    bool cmdRender(VkCommandBuffer commandBuffer, const std::shared_ptr<media::VideoFrame>& frame, Sync& sync);

    // The image, in the shader read only layout once the commands recorded
    // by cmdRender() are executed. Null before the first frame.
    const GPU::Image* image() const { return _image.get(); }
    uint32_t width() const { return _image ? _image->width() : 0; }
    uint32_t height() const { return _image ? _image->height() : 0; }

private:
    void createPipeline(const GPU::PipelineCache& pipelineCache);
    void ensureImage(uint32_t width, uint32_t height);

    bool cmdConvertHardwareFrame(VkCommandBuffer commandBuffer, const media::VideoFrame& frame, Sync& sync);
    void cmdUploadSoftwareFrame(VkCommandBuffer commandBuffer, const media::VideoFrame& frame);

    const GPU::Device& _device;

    VkSampler _sampler = VK_NULL_HANDLE;
    VkDescriptorSetLayout _descriptorSetLayout = VK_NULL_HANDLE;
    VkPipelineLayout _pipelineLayout = VK_NULL_HANDLE;
    VkPipeline _pipeline = VK_NULL_HANDLE;
    VkDescriptorPool _descriptorPool = VK_NULL_HANDLE;

    std::unique_ptr<GPU::Image> _image;
    std::unique_ptr<GPU::Buffer> _staging;

    // The frame in the image, and the resources of the frame in flight.
    std::shared_ptr<media::VideoFrame> _shownFrame;
    std::vector<std::shared_ptr<media::VideoFrame>> _framesInFlight;
    std::vector<VkImageView> _viewsInFlight;

    bool _reportedUnsupported = false;
};

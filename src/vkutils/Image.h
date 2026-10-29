#pragma once

#include <vulkan/vulkan.h>

#include <vkutils/Device.h>
#include <vkutils/Buffer.h>


namespace GPU
{
    class Image
    {
    public:
        Image(
            const Device& device,
            VkImageUsageFlags usage,
            VkFormat format,
            uint32_t width, uint32_t height,
            uint32_t mips = 1,
            uint32_t arrayLayers = 1,
            bool isCubeMap = false,
            VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT,
            VkSharingMode sharingMode = VK_SHARING_MODE_EXCLUSIVE,
            std::vector<uint32_t> iQueueFamilyIdx = {}
        );

        Image(
            const char* debugName,
            const Device& device,
            VkImageUsageFlags usage,
            VkFormat format,
            uint32_t width, uint32_t height,
            uint32_t mips = 1,
            uint32_t arrayLayers = 1,
            bool isCubeMap = false,
            VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT,
            VkSharingMode sharingMode = VK_SHARING_MODE_EXCLUSIVE,
            std::vector<uint32_t> iQueueFamilyIdx = {}
        );

        virtual ~Image();

        VkImage image() const { return _image; }
        VkImageView imageView() const { return _imageView; }
        VkImageSubresourceRange wholeRange() const { return _wholeRange; }
        VkFormat format() const { return _format; }

        uint32_t width() const { return _extent.width; }
        uint32_t height() const { return _extent.height; }

        // Texture size in bytes
        VkDeviceSize textureSize() const { return _textureSize; }

        VkImageMemoryBarrier& imageBarrier() { return _imageBarrier; }
        void setSrcStageMask(VkPipelineStageFlags srcStageMask) { _imageBarrierLastStageMask = srcStageMask; }

        VkPipelineStageFlags lastStageMask() const { return _imageBarrierLastStageMask; }

        void resetImageAccess();

        void cmdBarrierTransition(
            VkCommandBuffer commandBuffer,
            VkPipelineStageFlags dstStageMask,
            VkAccessFlags dstAccessMask,
            VkImageLayout newLayout
        );

        static void cmdBarrierTransition(
            std::vector<GPU::Image*> images,
            VkCommandBuffer commandBuffer,
            VkPipelineStageFlags dstStageMask,
            VkAccessFlags dstAccessMask,
            VkImageLayout newLayout
        );

    private:
        void initImage(
            VkImageUsageFlags usage,
            uint32_t mips,
            uint32_t arrayLayers,
            bool isCubeMap,
            VkSampleCountFlagBits samples,
            VkSharingMode sharingMode,
            std::vector<uint32_t> iQueueFamilyIdx
        );

        const Device& _device;

        VkImage _image = VK_NULL_HANDLE;
        VkImageView _imageView = VK_NULL_HANDLE;
        VkDeviceMemory _imageMemory = VK_NULL_HANDLE;

        VkImageSubresourceRange _wholeRange;
        VkFormat _format;
        VkExtent3D _extent;

        VkDeviceSize _textureSize = 0;

        VkImageMemoryBarrier _imageBarrier;
        VkPipelineStageFlags _imageBarrierLastStageMask = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    };
}

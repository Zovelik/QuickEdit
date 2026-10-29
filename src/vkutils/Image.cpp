#include "Image.h"

#include "util.h"

#include <cassert>

using namespace GPU;


Image::Image(
    const Device& device,
    VkImageUsageFlags usage,
    VkFormat format,
    uint32_t width, uint32_t height,
    uint32_t mips,
    uint32_t arrayLayers,
    bool isCubeMap,
    VkSampleCountFlagBits samples,
    VkSharingMode sharingMode,
    std::vector<uint32_t> iQueueFamilyIdx)
    : _device(device)
    , _format(format)
    , _extent({ width, height, 1 })
{
    initImage(
        usage,
        mips,
        arrayLayers,
        isCubeMap,
        samples,
        sharingMode,
        iQueueFamilyIdx
    );
}


Image::Image(
    const char* debugName,
    const Device& device,
    VkImageUsageFlags usage,
    VkFormat format,
    uint32_t width, uint32_t height,
    uint32_t mips,
    uint32_t arrayLayers,
    bool isCubeMap,
    VkSampleCountFlagBits samples,
    VkSharingMode sharingMode,
    std::vector<uint32_t> iQueueFamilyIdx)
    : _device(device)
    , _format(format)
    , _extent({ width, height, 1 })
{
    initImage(
        usage,
        mips,
        arrayLayers,
        isCubeMap,
        samples,
        sharingMode,
        iQueueFamilyIdx
    );

    _device.setObjectName(reinterpret_cast<uint64_t>(_image), VK_OBJECT_TYPE_IMAGE, debugName);
}



Image::~Image()
{
    if (_imageView)     vkDestroyImageView(_device.device(), _imageView, nullptr);
    if (_image)         vkDestroyImage(_device.device(), _image, nullptr);
    if (_imageMemory)   vkFreeMemory(_device.device(), _imageMemory, nullptr);
}


void Image::resetImageAccess()
{
    _imageBarrier.srcAccessMask = VK_ACCESS_NONE;
    _imageBarrier.dstAccessMask = VK_ACCESS_NONE;
    _imageBarrierLastStageMask = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
}


void Image::cmdBarrierTransition(
    VkCommandBuffer commandBuffer,
    VkPipelineStageFlags dstStageMask,
    VkAccessFlags dstAccessMask,
    VkImageLayout newLayout)
{
    _imageBarrier.dstAccessMask = dstAccessMask;
    _imageBarrier.newLayout = newLayout;

    vkCmdPipelineBarrier(
        commandBuffer,
        _imageBarrierLastStageMask,  // srcStageMask
        dstStageMask,               // dstStageMask
        0,                          // dependencyFlags
        0, nullptr,                 // pMemoryBarriers
        0, nullptr,                 // pBufferMemoryBarriers
        1, &_imageBarrier           // pImageMemoryBarriers
    );

    _imageBarrier.srcAccessMask = dstAccessMask;
    _imageBarrier.oldLayout = newLayout;
    _imageBarrierLastStageMask = dstStageMask;
}


void Image::cmdBarrierTransition(
    std::vector<GPU::Image*> images,
    VkCommandBuffer commandBuffer,
    VkPipelineStageFlags dstStageMask,
    VkAccessFlags dstAccessMask,
    VkImageLayout newLayout)
{
    // Try to combine the barriers if all images have the same source 
    // stage mask
    VkPipelineStageFlags srcStageMask = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    bool commonSrcStageMask = true;
    std::vector<VkImageMemoryBarrier> imageBarriers(images.size());

    for (size_t i = 0; i < images.size(); ++i) {
        images[i]->_imageBarrier.dstAccessMask = dstAccessMask;
        images[i]->_imageBarrier.newLayout = newLayout;

        if (i == 0) {
            srcStageMask = images[i]->_imageBarrierLastStageMask;
        }
        else if (images[i]->_imageBarrierLastStageMask != srcStageMask) {
            // If the source stage mask is different, we cannot combine the
            // barriers
            commonSrcStageMask = false;
            break;
        }

        imageBarriers[i] = images[i]->_imageBarrier;
    }

    if (commonSrcStageMask) {
        vkCmdPipelineBarrier(
            commandBuffer,
            srcStageMask,               // srcStageMask
            dstStageMask,               // dstStageMask
            0,                          // dependencyFlags
            0, nullptr,                 // pMemoryBarriers
            0, nullptr,                 // pBufferMemoryBarriers
            static_cast<uint32_t>(imageBarriers.size()),
            imageBarriers.data()        // pImageMemoryBarriers
        );

        for (size_t i = 0; i < images.size(); ++i) {
            images[i]->_imageBarrier.srcAccessMask = dstAccessMask;
            images[i]->_imageBarrier.oldLayout = newLayout;
            images[i]->_imageBarrierLastStageMask = dstStageMask;
        }
    }
    else {
        // Combine barriers is not possible, so we need to issue separate
        // barriers for each image
        for (size_t i = 0; i < images.size(); ++i) {
            images[i]->cmdBarrierTransition(
                commandBuffer,
                dstStageMask,
                dstAccessMask,
                newLayout
            );
        }
    }
}


void Image::initImage(
    VkImageUsageFlags usage,
    uint32_t mips,
    uint32_t arrayLayers,
    bool isCubeMap,
    VkSampleCountFlagBits samples,
    VkSharingMode sharingMode,
    std::vector<uint32_t> iQueueFamilyIdx)
{
    // Create the image & allocate its memory
    const VkImageCreateInfo imageCreateInfo = {
        VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, nullptr,               // sType, pNext
        isCubeMap ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0u,       // flags
        VK_IMAGE_TYPE_2D,                                           // imageType
        _format,                                                    // format
        _extent,                                                    // extent
        mips, arrayLayers,                                          // mipLevels, arrayLayers
        samples,                                                    // samples
        VK_IMAGE_TILING_OPTIMAL,                                    // tiling
        usage,                                                      // usage
        sharingMode,                                                // sharingMode
        (uint32_t)iQueueFamilyIdx.size(), iQueueFamilyIdx.data(),   // queueFamilyIndices
        VK_IMAGE_LAYOUT_UNDEFINED
    };

    VK_THROW_IF_FAILED(vkCreateImage(_device.device(), &imageCreateInfo, nullptr, &_image));

    VkMemoryRequirements memoryRequirements;
    vkGetImageMemoryRequirements(_device.device(), _image, &memoryRequirements);

    _textureSize = memoryRequirements.size;

    const VkMemoryAllocateInfo memoryAllocateInfo = {
        VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, 0,
        _textureSize,
        chooseMemoryTypeFromFlags(_device.physicalDevice(), memoryRequirements, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
    };

    VK_THROW_IF_FAILED(vkAllocateMemory(_device.device(), &memoryAllocateInfo, nullptr, &_imageMemory));

    VK_THROW_IF_FAILED(vkBindImageMemory(_device.device(), _image, _imageMemory, 0));

    VkImageAspectFlags aspectMaskWholeRange = VK_IMAGE_ASPECT_NONE;
    VkImageAspectFlags aspectMaskImageView = VK_IMAGE_ASPECT_NONE;

    if (usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) {
        switch (_format) {
        case VK_FORMAT_D16_UNORM:
        case VK_FORMAT_D32_SFLOAT:
            aspectMaskWholeRange = VK_IMAGE_ASPECT_DEPTH_BIT;
            break;
        case VK_FORMAT_D16_UNORM_S8_UINT:
        case VK_FORMAT_D24_UNORM_S8_UINT:
        case VK_FORMAT_D32_SFLOAT_S8_UINT:
            aspectMaskWholeRange = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
            break;
        case VK_FORMAT_S8_UINT:
            aspectMaskWholeRange = VK_IMAGE_ASPECT_STENCIL_BIT;
            break;
        }
        aspectMaskImageView = VK_IMAGE_ASPECT_DEPTH_BIT;
    }
    else {
        aspectMaskWholeRange = VK_IMAGE_ASPECT_COLOR_BIT;
        aspectMaskImageView = VK_IMAGE_ASPECT_COLOR_BIT;
    }

    // Create image view
    _wholeRange = {
        aspectMaskWholeRange,           // aspectMask
        0, mips,                        // baseMipLevel, levelCount
        0, arrayLayers                  // baseArrayLayer, layerCount
    };

    const VkImageViewCreateInfo imageViewCreateInfo = {
        VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, nullptr, 0,           // sType, pNext, flags
        _image,                                                         // image
        isCubeMap ? VK_IMAGE_VIEW_TYPE_CUBE : VK_IMAGE_VIEW_TYPE_2D,    // viewType
        _format,                                                        // format
        // Components
        {
            VK_COMPONENT_SWIZZLE_IDENTITY,
            VK_COMPONENT_SWIZZLE_IDENTITY,
            VK_COMPONENT_SWIZZLE_IDENTITY,
            VK_COMPONENT_SWIZZLE_IDENTITY,
        },
        // subresourceRange
        {
            aspectMaskImageView,            // aspectMask
            0, mips,                        // baseMipLevel, levelCount
            0, arrayLayers                  // baseArrayLayer, layerCount
        }
    };

    VK_THROW_IF_FAILED(vkCreateImageView(_device.device(), &imageViewCreateInfo, nullptr, &_imageView));

    // Create a default image barrier for this image
    _imageBarrier = {
           VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, nullptr,
           VK_ACCESS_NONE,                                      // srcAccessMask
           VK_ACCESS_NONE,                                      // dstAccesMask
           VK_IMAGE_LAYOUT_UNDEFINED,                           // oldLayout
           VK_IMAGE_LAYOUT_UNDEFINED,                           // newLayout
           VK_QUEUE_FAMILY_IGNORED,                             // srcQueueFamilyIndex
           VK_QUEUE_FAMILY_IGNORED,                             // dstQueueFamilyIndex
           _image,                                              // image
           _wholeRange
    };
}
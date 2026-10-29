#include <vkutils/Swapchain.h>
#include <vkutils/util.h>

#include <stdexcept>
#include <sstream>

using namespace GPU;


Swapchain::Swapchain(
    const Device& device,
    VkSurfaceKHR surface,
    uint32_t width, uint32_t height,
    bool vsync,
    VkImageLayout optimalLayout)
    : _device(device)
    , _optimalLayout(VK_IMAGE_LAYOUT_PRESENT_SRC_KHR)
    , _surface(surface)
    , _swapchain(VK_NULL_HANDLE)
{
    updateSwapchain(width, height, vsync);
}


Swapchain::~Swapchain()
{
    //vkDeviceWaitIdle(_rDevice->device());

    for (uint64_t timelineValue : _imageTimelineValue) {
        if (timelineValue > 0) {
            const VkSemaphoreWaitInfo waitInfo = {
                VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, nullptr, // sType, pNext
                0,                                              // flags
                1, &_semaphoreTimeline,                         // semaphoreCount, pSemaphores
                &timelineValue                                  // pValues
            };
            
            vkWaitSemaphores(_device.device(), &waitInfo, UINT64_MAX);
        }
    }

    for (uint32_t i = 0; i < _nSwapchainImages; i++) {
        vkDestroyImageView(_device.device(), _swapchainImageViews[i], nullptr);
        vkDestroySemaphore(_device.device(), _semaphoreImageAcquired[i], nullptr);
        vkDestroySemaphore(_device.device(), _semaphoreRenderReady[i], nullptr);
    }

    vkDestroySemaphore(_device.device(), _semaphoreTimeline, nullptr);

    vkDestroySwapchainKHR(_device.device(), _swapchain, nullptr);
}


void Swapchain::updateSwapchain(uint32_t width, uint32_t height, bool vsync)
{
    for (uint64_t timelineValue : _imageTimelineValue) {
        if (timelineValue > 0) {
            const VkSemaphoreWaitInfo waitInfo = {
                VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, nullptr, // sType, pNext
                0,                                              // flags
                1, &_semaphoreTimeline,                         // semaphoreCount, pSemaphores
                &timelineValue                                  // pValues
            };
            VK_THROW_IF_FAILED(vkWaitSemaphores(_device.device(), &waitInfo, UINT64_MAX));
        }
    }

    VkSwapchainKHR oldSwapchain = _swapchain;

    // Find surface capabilties
    VkSurfaceCapabilitiesKHR surfaceCapabilities;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(_device.physicalDevice(), _surface, &surfaceCapabilities);

    uint32_t nFormats;
    std::vector<VkSurfaceFormatKHR> supportedFormats;

    vkGetPhysicalDeviceSurfaceFormatsKHR(_device.physicalDevice(), _surface, &nFormats, nullptr);
    supportedFormats.resize(nFormats);
    vkGetPhysicalDeviceSurfaceFormatsKHR(_device.physicalDevice(), _surface, &nFormats, supportedFormats.data());

    bool formatFound = false;

    const std::vector<VkFormat> preferred = {
        VK_FORMAT_B8G8R8A8_UNORM,
        VK_FORMAT_R8G8B8A8_UNORM,
        //VK_FORMAT_B8G8R8A8_SRGB,
        //VK_FORMAT_R8G8B8A8_SRGB
    };

    for (const VkSurfaceFormatKHR& format : supportedFormats) {
        for (const auto& f : preferred) {
            if ((format.colorSpace & VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR &&
                format.format == f)
            {
                _swapchainFormat = format;
                formatFound = true;
                break;
            }
        }

        if (formatFound) {
            break;
        }
    }

    if (!formatFound) {
        throw std::runtime_error("[VULKAN] Failed to create swapchain: could not find a suitable format");
    }

    // Great... worked seamlessly with WIN32 and X with
    // surfaceCapabilities.currentExtent.width & height but somehow
    // broken on this damn Wayland...
    _format = _swapchainFormat.format;
    _width  = std::max(surfaceCapabilities.minImageExtent.width,  std::min(surfaceCapabilities.maxImageExtent.width,  width));
    _height = std::max(surfaceCapabilities.minImageExtent.height, std::min(surfaceCapabilities.maxImageExtent.height, height));
    _minImages = surfaceCapabilities.minImageCount;

    if (_width == UINT32_MAX || _height == UINT32_MAX) {
        // Wayland seems to return crazy value when creating the window
        return;
    }

    const VkSwapchainCreateInfoKHR swapchainCreateInfo = {
        VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, nullptr, 0,// sType, pNext, flags
        _surface,                                               // surface
        surfaceCapabilities.minImageCount,                      // minImageCount
        _format,                                                // imageFormat
        _swapchainFormat.colorSpace,                            // imageColorSpace
        { _width, _height },//surfaceCapabilities.currentExtent,// imageExtent
        1,                                                      // imageArrayLayers
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,                    // imageUsage
        VK_SHARING_MODE_EXCLUSIVE,                              // imageSharingMode
        0, nullptr,                                             // sharingQueues
        surfaceCapabilities.currentTransform,                   // preTransform
        chooseCompositeAlpha(surfaceCapabilities),              // compositeAlpha
        vsync ? VK_PRESENT_MODE_FIFO_KHR : VK_PRESENT_MODE_IMMEDIATE_KHR, // presentMode
        VK_FALSE,                                               // clipped
        oldSwapchain                                            // oldSwapchain
    };

    VK_THROW_IF_FAILED(vkCreateSwapchainKHR(_device.device(), &swapchainCreateInfo, nullptr, &_swapchain));

    if (oldSwapchain != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(_device.device(), oldSwapchain, nullptr);
    }

    // Get images
    vkGetSwapchainImagesKHR(_device.device(), _swapchain, &_nSwapchainImages, nullptr);
    _swapchainImages.resize(_nSwapchainImages);
    vkGetSwapchainImagesKHR(_device.device(), _swapchain, &_nSwapchainImages, _swapchainImages.data());

    initSwapchainResources();

    _iCurrFrame = 0;
    _valid = true;
}


void Swapchain::initSwapchainResources()
{
    // Free resources
    for (VkImageView& imageView : _swapchainImageViews) {
        vkDestroyImageView(_device.device(), imageView, nullptr);
        imageView = VK_NULL_HANDLE;
    }

    for (VkSemaphore& semaphore : _semaphoreImageAcquired) {
        vkDestroySemaphore(_device.device(), semaphore, nullptr);
        semaphore = VK_NULL_HANDLE;
    }

    for (VkSemaphore& semaphore : _semaphoreRenderReady) {
        vkDestroySemaphore(_device.device(), semaphore, nullptr);
        semaphore = VK_NULL_HANDLE;
    }

    vkDestroySemaphore(_device.device(), _semaphoreTimeline, nullptr);

    // Create imageviews
    _swapchainImageViews.resize(_nSwapchainImages);

    for (uint32_t i = 0; i < _nSwapchainImages; i++) {
        const VkImageViewCreateInfo imageViewCreateInfo = {
            VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, nullptr, 0,   // sType, pNext, flags
            _swapchainImages[i],                                    // image
            VK_IMAGE_VIEW_TYPE_2D,                                  // viewType
            _format,                                                // format
            {   // components
                VK_COMPONENT_SWIZZLE_IDENTITY,
                VK_COMPONENT_SWIZZLE_IDENTITY,
                VK_COMPONENT_SWIZZLE_IDENTITY,
                VK_COMPONENT_SWIZZLE_IDENTITY,
            },
            {   // subresourceRange
                VK_IMAGE_ASPECT_COLOR_BIT,                          // aspect
                0, 1,                                               // mips
                0, 1,                                               // layers
            }
        };

        VK_THROW_IF_FAILED(vkCreateImageView(_device.device(), &imageViewCreateInfo, nullptr, &_swapchainImageViews[i]));
    }

    // Create synchronization tools
    const VkSemaphoreCreateInfo semaphoreBinaryCreateInfo = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, nullptr, 0 };

    _semaphoreImageAcquired.resize(_nSwapchainImages);
    _semaphoreRenderReady.resize(_nSwapchainImages);
    _imageTimelineValue.resize(_nSwapchainImages);

    for (uint32_t i = 0; i < _nSwapchainImages; i++) {
        VK_THROW_IF_FAILED(vkCreateSemaphore(_device.device(), &semaphoreBinaryCreateInfo, nullptr, &_semaphoreImageAcquired[i]));
        VK_THROW_IF_FAILED(vkCreateSemaphore(_device.device(), &semaphoreBinaryCreateInfo, nullptr, &_semaphoreRenderReady[i]));
        _imageTimelineValue[i] = 0;

        std::stringstream debugName;
        debugName << "Swapchain::semaphoreImageAcquired[" << i << "]";
        _device.setObjectName(reinterpret_cast<uint64_t>(_semaphoreImageAcquired[i]), VK_OBJECT_TYPE_SEMAPHORE, debugName.str().c_str());
        debugName.str(""); // Clear the stream
        debugName << "Swapchain::semaphoreRenderReady[" << i << "]";
        _device.setObjectName(reinterpret_cast<uint64_t>(_semaphoreRenderReady[i]), VK_OBJECT_TYPE_SEMAPHORE, debugName.str().c_str());
    }

    _timelineValue = 0;

    const VkSemaphoreTypeCreateInfo timelineCreateInfo = {
        VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO, nullptr,  // sType, pNext
        VK_SEMAPHORE_TYPE_TIMELINE,                             // semaphoreType
        0                                                       // initialValue
    };

    const VkSemaphoreCreateInfo semaphoreTimelineCreateInfo = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &timelineCreateInfo, 0 };

    VK_THROW_IF_FAILED(vkCreateSemaphore(_device.device(), &semaphoreTimelineCreateInfo, nullptr, &_semaphoreTimeline));
    _device.setObjectName(reinterpret_cast<uint64_t>(_semaphoreTimeline), VK_OBJECT_TYPE_SEMAPHORE, "Swapchain::semaphoreTimeline");
}


VkResult Swapchain::acquireNextImage(
    uint32_t* iSwapchainImage,
    VkSemaphore* semaphoreImageAcquired,
    VkSemaphore* semaphoreRenderReady,
    VkSemaphore* signalTimelineSemaphore,
    uint64_t* signalTimelineValue)
{
    if (!_valid) {
        return VK_ERROR_OUT_OF_DATE_KHR;
    }

    const VkResult result = vkAcquireNextImageKHR(
        _device.device(),
        _swapchain,
        UINT64_MAX,
        _semaphoreImageAcquired[_iCurrFrame],
        VK_NULL_HANDLE,
        &_iSwapchainImage
    );

    // Suboptimal still acquires an image and signals the semaphore: render
    // this frame and recreate the swapchain on the next one.
    if (result == VK_SUBOPTIMAL_KHR) {
        _valid = false;
    }
    else if (result != VK_SUCCESS) {
        _valid = false;
        return result;
    }

    // Wait if the image is still used by the GPU
    if (_imageTimelineValue[_iSwapchainImage] > 0) {
        const VkSemaphoreWaitInfo waitInfo = {
            VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, nullptr, // sType, pNext
            0,                                              // flags
            1, &_semaphoreTimeline,                         // semaphoreCount, pSemaphores
            &_imageTimelineValue[_iSwapchainImage]          // pValues
        };

        VK_THROW_IF_FAILED(vkWaitSemaphores(_device.device(), &waitInfo, UINT64_MAX));
    }

    _timelineValue++;
    _imageTimelineValue[_iSwapchainImage] = _timelineValue;
    
    *iSwapchainImage            = _iSwapchainImage;
    *semaphoreImageAcquired     = _semaphoreImageAcquired[_iCurrFrame];
    *semaphoreRenderReady       = _semaphoreRenderReady[_iSwapchainImage];
    *signalTimelineSemaphore    = _semaphoreTimeline;
    *signalTimelineValue        = _timelineValue;

    return VK_SUCCESS;
}


void Swapchain::present(VkQueue queue)
{
    const VkPresentInfoKHR presentInfo = {
        VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, nullptr,    // sType, pNext
        1, &_semaphoreRenderReady[_iSwapchainImage],    // waitSemaphore
        1, &_swapchain, &_iSwapchainImage,              // swapchains
        nullptr                                         // result
    };

    VkResult resultPresent = vkQueuePresentKHR(queue, &presentInfo);

    if (resultPresent == VK_ERROR_OUT_OF_DATE_KHR || resultPresent == VK_SUBOPTIMAL_KHR) {
        //updateSwapchain();
        _valid = false;
    }

    _iCurrFrame = (_iCurrFrame + 1) % _nSwapchainImages;
}



VkCompositeAlphaFlagBitsKHR Swapchain::chooseCompositeAlpha(const VkSurfaceCapabilitiesKHR& caps)
{
    if (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR) {
        return VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR;
    }
    
    if (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR) {
        return VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR;
    }

    if (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR) {
        return VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
    }

    return VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
}
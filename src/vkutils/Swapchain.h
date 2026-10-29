#pragma once

#include <vkutils/Device.h>
#include <vector>


namespace GPU
{
    class Swapchain
    {
    public:
        Swapchain(
            const Device& device,
            VkSurfaceKHR surface,
            uint32_t width, uint32_t height,
            bool vsync = true,
            VkImageLayout optimalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);

        virtual void updateSwapchain(uint32_t width, uint32_t height, bool vsync = true);
        virtual void initSwapchainResources();

        virtual ~Swapchain();

        uint32_t minImages() const { return _minImages; }
        uint32_t nSwapchainImages() const { return _nSwapchainImages; }

        virtual VkResult acquireNextImage(
            uint32_t* iSwapchainImage,
            VkSemaphore* semaphoreImageAcquired,
            VkSemaphore* semaphoreRenderReady,
            VkSemaphore* signalTimelineSemaphore,
            uint64_t* signalTimelineValue
        );

        virtual void present(VkQueue queue);

        VkSwapchainKHR swapchain() const { return _swapchain; }

        VkImage image(size_t i) const { return _swapchainImages[i]; }
        VkImageView imageView(size_t i) const { return _swapchainImageViews[i]; }

        VkFormat format() const { return _format; }
        VkImageLayout optimalLayout() const { return _optimalLayout; }

        uint32_t width() const { return _width; }
        uint32_t height() const { return _height; }

        bool valid() const { return _valid; }

        static VkCompositeAlphaFlagBitsKHR chooseCompositeAlpha(const VkSurfaceCapabilitiesKHR& caps);

    protected:
        const Device& _device;

        VkSurfaceKHR _surface;
        VkSwapchainKHR _swapchain;
        VkSurfaceFormatKHR _swapchainFormat;

        uint32_t _nSwapchainImages;
        uint32_t _iSwapchainImage;
        uint32_t _iCurrFrame = 0;

        std::vector<VkSemaphore> _semaphoreImageAcquired;
        std::vector<VkSemaphore> _semaphoreRenderReady;

        VkSemaphore _semaphoreTimeline = VK_NULL_HANDLE;
        uint64_t _timelineValue = 0;
        std::vector<uint64_t> _imageTimelineValue;

        std::vector<VkImage> _swapchainImages;
        std::vector<VkImageView> _swapchainImageViews;
        VkImageLayout _optimalLayout;
        VkFormat _format;
        uint32_t _width, _height;
        bool _valid = false;

        uint32_t _minImages;
    };
}
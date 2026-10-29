#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>
#include <functional>
#include <vector>

struct AVBufferRef;
struct AVHWDeviceContext;


namespace media
{
    // The Vulkan device of the application, as FFmpeg needs it to decode
    // (and later encode) on it: frames then stay on the GPU, as VkImages.
    struct VulkanDeviceDesc
    {
        VkInstance instance = VK_NULL_HANDLE;
        VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
        VkDevice device = VK_NULL_HANDLE;

        // Features enabled at device creation; must outlive the HwDevice.
        const VkPhysicalDeviceFeatures2* features = nullptr;

        std::vector<const char*> instanceExtensions;
        std::vector<const char*> deviceExtensions;

        struct QueueFamily
        {
            uint32_t index;
            uint32_t queueCount;    // Queues created in this family
            VkQueueFlags flags;
        };

        // Preferred first.
        std::vector<QueueFamily> queueFamilies;

        // Locks (lock = true) or unlocks the queue of the given family and
        // index, shared with the application's own submissions.
        std::function<void(uint32_t family, uint32_t index, bool lock)> lockQueue;
    };


    // FFmpeg hardware device context (AV_HWDEVICE_TYPE_VULKAN) wrapping the
    // application's device.
    class HwDevice
    {
    public:
        // Throws std::runtime_error when FFmpeg refuses the device.
        explicit HwDevice(const VulkanDeviceDesc& desc);
        ~HwDevice();

        HwDevice(const HwDevice&) = delete;
        HwDevice& operator=(const HwDevice&) = delete;

        // The AVBufferRef of the AVHWDeviceContext, to reference with
        // av_buffer_ref() for each decoder.
        AVBufferRef* context() const { return _context; }

        bool canDecodeVideo() const { return _canDecodeVideo; }

    private:
        static void lockQueue(::AVHWDeviceContext* context, uint32_t family, uint32_t index);
        static void unlockQueue(::AVHWDeviceContext* context, uint32_t family, uint32_t index);

        VulkanDeviceDesc _desc;
        AVBufferRef* _context = nullptr;
        bool _canDecodeVideo = false;
    };
}

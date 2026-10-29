#pragma once

#include <vector>
#include <vulkan/vulkan.h>


namespace GPU
{
    class Device
    {
    public:
        Device(
            VkInstance instance,
            VkPhysicalDevice physicalDevice,
            const std::vector<VkQueueFlags>& requestedQueues,
            const std::vector<const char*>& deviceExtensions = {},
            const void* extraFeatures = nullptr,
            const VkPhysicalDeviceFeatures* coreFeatures = nullptr
        );

        Device(
            VkInstance instance,
            VkPhysicalDevice physicalDevice,
            std::vector<std::pair<uint32_t, uint32_t>> queueFamilyIdxCount,
            std::vector<std::pair<uint32_t, uint32_t>> queueFamilyIdx,
            const std::vector<const char*>& deviceExtensions = {},
            const void* extraFeatures = nullptr,
            const VkPhysicalDeviceFeatures* coreFeatures = nullptr
        );

        // Device managed by the caller
        Device(
            VkInstance instance,
            VkPhysicalDevice physicalDevice,
            VkDevice device,
            const std::vector <std::pair<uint32_t, uint32_t>>& queuesFamilyIdx
        );

        virtual ~Device();

        VkPhysicalDevice physicalDevice() const { return _physicalDevice; }
        VkDevice device() const { return _device; }
        VkInstance instance() const { return _rInstance; }

        const std::vector<uint32_t> queueFamilyIndices() const { return _queueFamilyIdx; }
        uint32_t queueFamilyIdx(uint32_t iQueue) const { return _queueFamilyIdx[iQueue]; }
        VkQueue queue(uint32_t iQueue) const { return _queues[iQueue]; }

        static bool deviceSupportsExtensions(VkPhysicalDevice physicalDevice, const std::vector<const char*>& deviceExtensions);

        void setObjectName(uint64_t handle, VkObjectType type, const char* name) const;

    private:
        bool _selfManagedDevice;

        VkInstance _rInstance            = VK_NULL_HANDLE;
        VkPhysicalDevice _physicalDevice = VK_NULL_HANDLE;
        VkDevice _device                 = VK_NULL_HANDLE;

        std::vector<uint32_t> _queueFamilyIdx;
        std::vector<VkQueue> _queues;

#ifndef NDEBUG
        PFN_vkSetDebugUtilsObjectNameEXT vkSetDebugUtilsObjectNameEXT = nullptr;
#endif
    };
}
#pragma once

#include <vector>
#include <vulkan/vulkan.h>


namespace GPU
{
    class Instance
    {
    public:
        Instance(
            const std::vector<const char*>& enabledLayers = {},
            const std::vector<const char*>& instanceExtensions = {});

        // Instance managed by the caller
        Instance(VkInstance instance);

        virtual ~Instance();

        VkInstance instance() const { return _instance; }

        VkPhysicalDevice findDevice(
            const std::vector<VkQueueFlags>& requestedQueues,
            const std::vector<const char*>& extensionNames = {},
            VkSurfaceKHR surface = VK_NULL_HANDLE
        ) const;

        void getAllPhysicalDevices(std::vector<VkPhysicalDevice>& physicalDevices) const;
        void getPhysicalDevicesWithExtensions(
            std::vector<VkPhysicalDevice>& physicalDevices,
            const std::vector<const char*>& extensionNames) const;

        static bool findQueueFamilies(
            VkPhysicalDevice physicalDevice,
            const std::vector<VkQueueFlags>& requestedQueues,
            std::vector<std::pair<uint32_t, uint32_t>>& queueFamilyIdxCount,
            std::vector<std::pair<uint32_t, uint32_t>>& requestedFamilyIdx,
            bool limitQueueCount = false,
            VkSurfaceKHR surface = VK_NULL_HANDLE
        );

        uint32_t api_version() const { return API_VERSION; }

    private:
        bool _selfManagedInstance;

        VkInstance _instance = VK_NULL_HANDLE;

    public:
        static const uint32_t API_VERSION = VK_API_VERSION_1_4;
    };
}
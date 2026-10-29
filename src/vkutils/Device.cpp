#include <vkutils/Device.h>
#include <vkutils/Instance.h>
#include <vkutils/util.h>

#include <iostream>
#include <cassert>
#include <cstring>

using namespace GPU;


Device::Device(
    VkInstance instance,
    VkPhysicalDevice physicalDevice,
    const std::vector<VkQueueFlags>& requestedQueues,
    const std::vector<const char*>& deviceExtensions,
    const void* extraFeatures,
    const VkPhysicalDeviceFeatures* coreFeatures)
    : _rInstance(instance)
    , _selfManagedDevice(true)
    , _physicalDevice(physicalDevice)
{
    // Look for queue family
    std::vector<std::pair<uint32_t, uint32_t>> queueFamilyIdxCount;
    std::vector<std::pair<uint32_t, uint32_t>> requestedFamilyIdx;

    bool queueFound = false;
    
    for (bool limitQueueCount : { false, true }) {
        queueFound = Instance::findQueueFamilies(
            _physicalDevice,
            requestedQueues,
            queueFamilyIdxCount,
            requestedFamilyIdx,
            limitQueueCount);

        if (queueFound) {
            break;
        }
    }

    if (!queueFound) {
        throw std::runtime_error("Could not find requested queues on selected device");
    }

    // Populate queueCreateInfo with matching queues
    std::vector<VkDeviceQueueCreateInfo> queuesCreateInfo;
    std::vector<std::vector<float>> queuesPriorities;

    for (const auto& familyIdxCount: queueFamilyIdxCount) {
        const uint32_t iQueueFamily = familyIdxCount.first;
        const uint32_t queueCount = familyIdxCount.second;

        queuesPriorities.push_back(std::vector<float>(queueCount, 1.f));
        queuesCreateInfo.push_back(
            {
                VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, nullptr, 0, // sType, pNext, flags
                iQueueFamily,                                           // queueFamilyIndex
                queueCount, queuesPriorities.back().data()              // queueCount, priorities
            }
        );
    }

    const VkDeviceCreateInfo deviceCreateInfo = {
        VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,                           // sType
        extraFeatures,                                                  // pNext
        0,                                                              // flags
        (uint32_t)queuesCreateInfo.size(), queuesCreateInfo.data(),     // pQueueCreateInfos
        0, NULL,                                                        // enabledLayers (deprecated)
        (uint32_t)deviceExtensions.size(), deviceExtensions.data(),     // enabledExtensions
        coreFeatures                                                    // enabledFeatures
    };

    VK_THROW_IF_FAILED(vkCreateDevice(_physicalDevice, &deviceCreateInfo, nullptr, &_device));

    // Retrieve queues from the device
    for (const auto& foundQueueFamily: requestedFamilyIdx) {
        const uint32_t& iQueueFamily = foundQueueFamily.first;
        const uint32_t& iQueueIdx    = foundQueueFamily.second;

        VkQueue queue;
        vkGetDeviceQueue(_device, iQueueFamily, iQueueIdx, &queue);

        _queueFamilyIdx.push_back(iQueueFamily);
        _queues.push_back(queue);
    }

    assert(requestedQueues.size() == _queueFamilyIdx.size());
    assert(requestedQueues.size() == _queues.size());

#ifndef NDEBUG
    vkSetDebugUtilsObjectNameEXT = (PFN_vkSetDebugUtilsObjectNameEXT)vkGetInstanceProcAddr(instance, "vkSetDebugUtilsObjectNameEXT");
#endif
}


Device::Device(
    VkInstance instance,
    VkPhysicalDevice physicalDevice,
    std::vector<std::pair<uint32_t, uint32_t>> queueFamilyIdxCount,
    std::vector<std::pair<uint32_t, uint32_t>> queueFamilyIdx,
    const std::vector<const char*>& deviceExtensions,
    const void* extraFeatures,
    const VkPhysicalDeviceFeatures* coreFeatures)
    : _rInstance(instance)
    , _selfManagedDevice(true)
    , _physicalDevice(physicalDevice)
{
    // Populate queueCreateInfo with matching queues
    std::vector<VkDeviceQueueCreateInfo> queuesCreateInfo;
    std::vector<std::vector<float>> queuesPriorities;

    for (const auto& familyIdxCount : queueFamilyIdxCount) {
        const uint32_t iQueueFamily = familyIdxCount.first;
        const uint32_t queueCount = familyIdxCount.second;

        queuesPriorities.push_back(std::vector<float>(queueCount, 1.f));
        queuesCreateInfo.push_back(
            {
                VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, nullptr, 0, // sType, pNext, flags
                iQueueFamily,                                           // queueFamilyIndex
                queueCount, queuesPriorities.back().data()              // queueCount, priorities
            }
        );
    }

    const VkDeviceCreateInfo deviceCreateInfo = {
        VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,                           // sType
        extraFeatures,                                                  // pNext
        0,                                                              // flags
        (uint32_t)queuesCreateInfo.size(), queuesCreateInfo.data(),     // pQueueCreateInfos
        0, NULL,                                                        // enabledLayers (deprecated)
        (uint32_t)deviceExtensions.size(), deviceExtensions.data(),     // enabledExtensions
        coreFeatures                                                    // enabledFeatures
    };

    VK_THROW_IF_FAILED(vkCreateDevice(_physicalDevice, &deviceCreateInfo, nullptr, &_device));

    // Retrieve queues from the device
    for (const auto& foundQueueFamily : queueFamilyIdx) {
        const uint32_t& iQueueFamily = foundQueueFamily.first;
        const uint32_t& iQueueIdx = foundQueueFamily.second;

        VkQueue queue;
        vkGetDeviceQueue(_device, iQueueFamily, iQueueIdx, &queue);

        _queueFamilyIdx.push_back(iQueueFamily);
        _queues.push_back(queue);
    }

#ifndef NDEBUG
    vkSetDebugUtilsObjectNameEXT = (PFN_vkSetDebugUtilsObjectNameEXT)vkGetInstanceProcAddr(instance, "vkSetDebugUtilsObjectNameEXT");
#endif
}


Device::Device(
    VkInstance instance,
    VkPhysicalDevice physicalDevice,
    VkDevice device,
    const std::vector <std::pair<uint32_t, uint32_t>>& queuesFamilyIdx)
    : _rInstance(instance)
    , _selfManagedDevice(false)
    , _physicalDevice(physicalDevice)
    , _device(device)
{
    // Retrieve queues from the device
    for (const auto& queueFamilyIdx : queuesFamilyIdx) {
        const uint32_t& iQueueFamily = queueFamilyIdx.first;
        const uint32_t& iQueueIdx = queueFamilyIdx.second;

        VkQueue queue;
        vkGetDeviceQueue(_device, iQueueFamily, iQueueIdx, &queue);

        _queueFamilyIdx.push_back(iQueueFamily);
        _queues.push_back(queue);
    }

#ifndef NDEBUG
    vkSetDebugUtilsObjectNameEXT = (PFN_vkSetDebugUtilsObjectNameEXT)vkGetInstanceProcAddr(instance, "vkSetDebugUtilsObjectNameEXT");
#endif
}


Device::~Device()
{
    // We own the device
    if (_selfManagedDevice) {
        vkDeviceWaitIdle(device());

        if (_device)   vkDestroyDevice(_device, nullptr);
    }
}


bool Device::deviceSupportsExtensions(VkPhysicalDevice physicalDevice, const std::vector<const char*>& deviceExtensions)
{
    uint32_t nExtensions;
    std::vector<VkExtensionProperties> extensions;
    vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &nExtensions, nullptr);
    extensions.resize(nExtensions);
    vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &nExtensions, extensions.data());

    size_t found = 0;

    for (const char* reqExt : deviceExtensions) {
        for (const auto& extProp : extensions) {
            if (std::strcmp(extProp.extensionName, reqExt) == 0) {
                ++found;
                break;
            }
        }
    }

    return found == deviceExtensions.size();
}


void Device::setObjectName(uint64_t handle, VkObjectType type, const char* name) const
{
#ifndef NDEBUG
    const VkDebugUtilsObjectNameInfoEXT info = {
        VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT, nullptr,    // sType, pNext
        type,                                                           // objectType
        handle,                                                         // objectHandle
        name                                                            // pObjectName
    };
    
    vkSetDebugUtilsObjectNameEXT(_device, &info);
#endif
}
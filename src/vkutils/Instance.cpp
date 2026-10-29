#include "Instance.h"
#include "util.h"

#include <config.h>
#include <iostream>
#include <cstring>

using namespace GPU;


Instance::Instance(
    const std::vector<const char*>& enabledLayers,
    const std::vector<const char*>& instanceExtensions)
    : _selfManagedInstance(true)
{
    const VkApplicationInfo applicationInfo = {
        VK_STRUCTURE_TYPE_APPLICATION_INFO, nullptr,    // sType, pNext
        APP_NAME_STR,                                   // pApplicationName
        VK_MAKE_VERSION(0, 1, 0),                       // applicationVersion
        APP_NAME_STR,                                   // pEngineName
        VK_MAKE_VERSION(1, 0, 0),                       // engineVersion
        API_VERSION                                     // apiVersion
    };

    std::vector<const char*> allEnabledLayers(enabledLayers);

    const VkInstanceCreateInfo instanceCreateInfo = {
        VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, nullptr, 0,             // sType, pNext, flags
        &applicationInfo,                                               // applicationInfo
        (uint32_t)allEnabledLayers.size(), allEnabledLayers.data(),     // Layers
        (uint32_t)instanceExtensions.size(), instanceExtensions.data()  // Extensions
    };

    VK_THROW_IF_FAILED(vkCreateInstance(&instanceCreateInfo, nullptr, &_instance));
}


Instance::Instance(VkInstance instance)
    : _selfManagedInstance(false)
    , _instance(instance)
{
}


Instance::~Instance()
{
    if (_selfManagedInstance) {
        if (_instance) vkDestroyInstance(_instance, nullptr);
    }
}


VkPhysicalDevice Instance::findDevice(
    const std::vector<VkQueueFlags>& requestedQueues,
    const std::vector<const char*>& extensionNames,
    VkSurfaceKHR surface) const
{
    std::vector<VkPhysicalDevice> physicalDevices;
    getPhysicalDevicesWithExtensions(physicalDevices, extensionNames);

    // TODO for testing
    //return physicalDevices[1];

    std::vector<std::pair<uint32_t, uint32_t>> queueFamilyIdxCount;
    std::vector<std::pair<uint32_t, uint32_t>> requestedFamilyIdx;

    for (bool limitQueueCount : { false, true }) {
        for (VkPhysicalDevice& currDevice : physicalDevices) {
            const bool queuesFound = findQueueFamilies(
                currDevice,
                requestedQueues,
                queueFamilyIdxCount,
                requestedFamilyIdx,
                limitQueueCount,
                surface
            );

            if (queuesFound) {
                return currDevice;
            }
        }
    }

    return VK_NULL_HANDLE;
}


void Instance::getAllPhysicalDevices(std::vector<VkPhysicalDevice>& physicalDevices) const
{
    uint32_t nPhysicalDevices = 0;
    vkEnumeratePhysicalDevices(_instance, &nPhysicalDevices, nullptr);

    physicalDevices.clear();

    if (nPhysicalDevices > 0) {
        physicalDevices.resize(nPhysicalDevices);
        vkEnumeratePhysicalDevices(_instance, &nPhysicalDevices, physicalDevices.data());
    }
}


void Instance::getPhysicalDevicesWithExtensions(
    std::vector<VkPhysicalDevice>& physicalDevices,
    const std::vector<const char*>& extensionNames) const
{
    std::vector<VkPhysicalDevice> allPhysicalDevices;
    getAllPhysicalDevices(allPhysicalDevices);

    physicalDevices.clear();

    for (VkPhysicalDevice& physicalDevice : allPhysicalDevices) {
        uint32_t nExtensions;
        std::vector<VkExtensionProperties> extensions;
        vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &nExtensions, nullptr);

        if (nExtensions > 0) {
            extensions.resize(nExtensions);
            vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &nExtensions, extensions.data());

            // check amongst physical device extension if all provided extensionNames
            // are available
            uint32_t found = 0;

            for (const char* reqExt : extensionNames) {
                for (const auto& extProp : extensions) {
                    if (std::strcmp(extProp.extensionName, reqExt) == 0) {
                        ++found;
                        break;
                    }
                }
            }

            if (found == extensionNames.size()) {
                physicalDevices.push_back(physicalDevice);
            }
        }
    }
}


bool Instance::findQueueFamilies(
    VkPhysicalDevice physicalDevice,
    const std::vector<VkQueueFlags>& requestedQueues,
    std::vector<std::pair<uint32_t, uint32_t>>& queueFamilyIdxCount,
    std::vector<std::pair<uint32_t, uint32_t>>& requestedFamilyIdx,
    bool limitQueueCount,
    VkSurfaceKHR surface)
{
    uint32_t nQueueFamilies = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &nQueueFamilies, nullptr);
    std::vector<VkQueueFamilyProperties> queueProperties(nQueueFamilies);
    vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &nQueueFamilies, queueProperties.data());

    queueFamilyIdxCount.clear();
    requestedFamilyIdx.resize(requestedQueues.size());

    std::vector<bool> queueAllocated(requestedQueues.size(), false);
    int nQueuesAllocated = 0;

    for (uint32_t iQueueFamily = 0; iQueueFamily < nQueueFamilies; iQueueFamily++) {
        for (size_t iRequestedQueue = 0; iRequestedQueue < requestedQueues.size(); iRequestedQueue++) {
            // Skip this queue, already allocated
            if (queueAllocated[iRequestedQueue]) {
                continue;
            }

            const VkQueueFlags& requiredFlags = requestedQueues[iRequestedQueue];
            bool suitableQueue = (queueProperties[iQueueFamily].queueFlags & requiredFlags) == requiredFlags;

            // If we require to present to a surface, check if the graphics queue supports it
            if (suitableQueue &&
                surface != VK_NULL_HANDLE && 
                requiredFlags & VK_QUEUE_GRAPHICS_BIT) {
                VkBool32 presentSupport = VK_FALSE;
                vkGetPhysicalDeviceSurfaceSupportKHR(physicalDevice, iQueueFamily, surface, &presentSupport);
                suitableQueue = presentSupport == VK_TRUE;
            }

            if (suitableQueue) {
                // Check if we already used this queue familly for the previous requested queue
                if (queueFamilyIdxCount.size() > 0 && queueFamilyIdxCount.back().first == iQueueFamily) {
                    // Selection startegy change: on iGPU, we may have a single queue familly with a single
                    // queue, whereas on dGPU, we may want to allocate multiple queues
                    if (limitQueueCount) {
                        queueFamilyIdxCount.back().second = std::min(
                            queueProperties[iQueueFamily].queueCount,
                            queueFamilyIdxCount.back().second + 1);
                    }
                    else {
                        if (queueFamilyIdxCount.back().second == queueProperties[iQueueFamily].queueCount) {
                            // We've allocated already the max number of queues on this queue familly,
                            // go to the next one
                            break;
                        }
                        else {
                            ++queueFamilyIdxCount.back().second;
                        }
                    }
                }
                else {
                    queueFamilyIdxCount.push_back(std::make_pair(iQueueFamily, 1));
                }

                // Save the requested queue family idx and position in this family
                requestedFamilyIdx[iRequestedQueue].first = queueFamilyIdxCount.back().first;
                requestedFamilyIdx[iRequestedQueue].second = queueFamilyIdxCount.back().second - 1;

                // We've allocated another queue, keep count to check if we could allocate all
                ++nQueuesAllocated;

                queueAllocated[iRequestedQueue] = true;
            }
        }
    }

    return nQueuesAllocated == requestedQueues.size();
}

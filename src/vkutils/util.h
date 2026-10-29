#pragma once

#include <stdexcept>
#include <string>
#include <filesystem>
#include <vector>

#include <vulkan/vulkan.h>


#define C_ARRAY_SIZE(_x) (sizeof(_x) / sizeof((_x[0])))


#define VK_THROW_IF_FAILED(_expr) \
    do { \
        VkResult _vk_result = (_expr); \
        if (_vk_result != VK_SUCCESS) { \
            throw std::runtime_error( \
                std::string("[VULKAN] Vulkan error: ") + std::to_string(_vk_result) + \
                " at " + __FILE__ + ":" + std::to_string(__LINE__)); \
        } \
    } while (0)


#define GetInstanceProcAddr(instance, name)                                                 \
    do {                                                                                    \
        name = reinterpret_cast<decltype(name)>(vkGetInstanceProcAddr((instance), #name));  \
    } while (0)


#define GetDeviceProcAddr(device, name)                                                     \
    do {                                                                                    \
        name = reinterpret_cast<decltype(name)>(vkGetDeviceProcAddr((device), #name));      \
    } while (0)


void loadFile(const std::filesystem::path& path, std::vector<char>& fileContent);

uint32_t chooseMemoryTypeFromFlags(
    const VkPhysicalDevice physicalDevice,
    const VkMemoryRequirements & memoryRequirements,
    VkMemoryPropertyFlags requiredFlags);

uint16_t floatToHalf(float f);

uint32_t get_el_with_padding(uint32_t n_el, uint32_t el_per_workgroup);
uint32_t get_n_workgroups(uint32_t n_el, uint32_t el_per_workgroup);

uint32_t maxMipsForResolution(uint32_t width, uint32_t height);

void imageBarrierDstToSrcFields(VkImageMemoryBarrier & barrier);

template<typename T>
VkObjectType getObjectType(T handle);

template<> inline VkObjectType getObjectType(VkInstance) { return VK_OBJECT_TYPE_INSTANCE; }
template<> inline VkObjectType getObjectType(VkPhysicalDevice) { return VK_OBJECT_TYPE_PHYSICAL_DEVICE; }
template<> inline VkObjectType getObjectType(VkDevice) { return VK_OBJECT_TYPE_DEVICE; }
template<> inline VkObjectType getObjectType(VkQueue) { return VK_OBJECT_TYPE_QUEUE; }
template<> inline VkObjectType getObjectType(VkSemaphore) { return VK_OBJECT_TYPE_SEMAPHORE; }
template<> inline VkObjectType getObjectType(VkCommandBuffer) { return VK_OBJECT_TYPE_COMMAND_BUFFER; }
template<> inline VkObjectType getObjectType(VkFence) { return VK_OBJECT_TYPE_FENCE; }
template<> inline VkObjectType getObjectType(VkDeviceMemory) { return VK_OBJECT_TYPE_DEVICE_MEMORY; }
template<> inline VkObjectType getObjectType(VkBuffer) { return VK_OBJECT_TYPE_BUFFER; }
template<> inline VkObjectType getObjectType(VkImage) { return VK_OBJECT_TYPE_IMAGE; }
template<> inline VkObjectType getObjectType(VkEvent) { return VK_OBJECT_TYPE_EVENT; }
template<> inline VkObjectType getObjectType(VkQueryPool) { return VK_OBJECT_TYPE_QUERY_POOL; }
template<> inline VkObjectType getObjectType(VkBufferView) { return VK_OBJECT_TYPE_BUFFER_VIEW; }
template<> inline VkObjectType getObjectType(VkImageView) { return VK_OBJECT_TYPE_IMAGE_VIEW; }
template<> inline VkObjectType getObjectType(VkShaderModule) { return VK_OBJECT_TYPE_SHADER_MODULE; }
template<> inline VkObjectType getObjectType(VkPipelineCache) { return VK_OBJECT_TYPE_PIPELINE_CACHE; }
template<> inline VkObjectType getObjectType(VkPipelineLayout) { return VK_OBJECT_TYPE_PIPELINE_LAYOUT; }
template<> inline VkObjectType getObjectType(VkRenderPass) { return VK_OBJECT_TYPE_RENDER_PASS; }
template<> inline VkObjectType getObjectType(VkPipeline) { return VK_OBJECT_TYPE_PIPELINE; }
template<> inline VkObjectType getObjectType(VkDescriptorSetLayout) { return VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT; }
template<> inline VkObjectType getObjectType(VkSampler) { return VK_OBJECT_TYPE_SAMPLER; }
template<> inline VkObjectType getObjectType(VkDescriptorPool) { return VK_OBJECT_TYPE_DESCRIPTOR_POOL; }
template<> inline VkObjectType getObjectType(VkDescriptorSet) { return VK_OBJECT_TYPE_DESCRIPTOR_SET; }
template<> inline VkObjectType getObjectType(VkFramebuffer) { return VK_OBJECT_TYPE_FRAMEBUFFER; }
template<> inline VkObjectType getObjectType(VkCommandPool) { return VK_OBJECT_TYPE_COMMAND_POOL; }
template<> inline VkObjectType getObjectType(VkSwapchainKHR) { return VK_OBJECT_TYPE_SWAPCHAIN_KHR; }
template<> inline VkObjectType getObjectType(VkSurfaceKHR) { return VK_OBJECT_TYPE_SURFACE_KHR; }


template<typename T>
void setObjectName(VkDevice device, T handle, const std::string & name)
{
#ifndef NDEBUG
    const VkDebugUtilsObjectNameInfoEXT nameInfo = {
        VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT,
        nullptr,                                            // pNext
        getObjectType(handle),                              // objectType
        reinterpret_cast<uint64_t>(handle),                 // objectHandle
        name.c_str()                                        // pObjectName
    };

    PFN_vkSetDebugUtilsObjectNameEXT vkSetDebugUtilsObjectNameEXT = reinterpret_cast<PFN_vkSetDebugUtilsObjectNameEXT>(vkGetDeviceProcAddr(device, "vkSetDebugUtilsObjectNameEXT"));

    if (vkSetDebugUtilsObjectNameEXT) {
        vkSetDebugUtilsObjectNameEXT(device, &nameInfo);
    }
#endif
}
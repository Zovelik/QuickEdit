#include "CommandPool.h"

#include "util.h"

using namespace GPU;


CommandPool::CommandPool(
    const Device& device,
    uint32_t queueFamilyIdx,
    uint32_t nCommandBuffers)
    : _device(device)
    , _commandBuffers(nCommandBuffers)
{
    const VkCommandPoolCreateInfo commandPoolCreateInfo = {
        VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, 0,  // sType, pNext
        0,                                              // flags
        queueFamilyIdx                                  // queueFamilyIndex
    };

    VK_THROW_IF_FAILED(vkCreateCommandPool(_device.device(), &commandPoolCreateInfo, nullptr, &_commandPool));

    const VkCommandBufferAllocateInfo commandBufferAllocateInfo = {
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, nullptr,    // sType, pNext
        _commandPool,                                               // commandPool
        VK_COMMAND_BUFFER_LEVEL_PRIMARY,                            // level
        nCommandBuffers                                             // commandBufferCount
    };

    VK_THROW_IF_FAILED(vkAllocateCommandBuffers(_device.device(), &commandBufferAllocateInfo, _commandBuffers.data()));
}


CommandPool::~CommandPool()
{
    vkFreeCommandBuffers(_device.device(), _commandPool, (uint32_t)_commandBuffers.size(), _commandBuffers.data());
    
    if (_commandPool) vkDestroyCommandPool(_device.device(), _commandPool, nullptr);
}
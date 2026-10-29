#pragma once

#include <vkutils/Device.h>


namespace GPU
{
    class CommandPool
    {
    public:
        CommandPool(
            const Device& device,
            uint32_t queueFamilyIdx,
            uint32_t nCommandBuffers
        );

        virtual ~CommandPool();

        VkCommandBuffer commandBuffer(uint32_t idx) { return _commandBuffers[idx]; }

    private:
        const Device& _device;

        VkCommandPool _commandPool = VK_NULL_HANDLE;
        std::vector<VkCommandBuffer> _commandBuffers;
    };
}
#pragma once

#include <vkutils/Device.h>

namespace GPU
{
    class DescriptorPool
    {
    public:
        DescriptorPool(
            const Device& device,
            const std::vector<std::vector<VkDescriptorSetLayoutBinding>>& bindings
        );

        ~DescriptorPool();

        VkDescriptorPool handle() const { return _descriptorPool; }

    private:
        const Device& _device;

        VkDescriptorPool _descriptorPool = VK_NULL_HANDLE;
    };
}
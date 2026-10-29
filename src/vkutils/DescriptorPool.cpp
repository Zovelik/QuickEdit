#include <vkutils/DescriptorPool.h>

#include <vkutils/util.h>

using namespace GPU;


DescriptorPool::DescriptorPool(
    const Device& device,
    const std::vector<std::vector<VkDescriptorSetLayoutBinding>>& bindings)
    : _device(device)
{
    // Flatten the bindings into a single vector of VkDescriptorPoolSize
    std::vector<VkDescriptorPoolSize> descriptorPoolSizes;

    for (const std::vector<VkDescriptorSetLayoutBinding>& bindingsPerSet : bindings) {
        for (const VkDescriptorSetLayoutBinding& binding : bindingsPerSet) {
            descriptorPoolSizes.push_back({
                binding.descriptorType,   // type
                binding.descriptorCount   // descriptorCount
                });
        }
    }

    if (descriptorPoolSizes.size() > std::numeric_limits<uint32_t>::max()) {
        throw std::runtime_error("Too many descriptor pool sizes");
    }

    const VkDescriptorPoolCreateInfo descriptorPoolCreateInfo = {
        VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, nullptr,     // sType, pNext
        0,                                                          // flags
        static_cast<uint32_t>(descriptorPoolSizes.size()),          // maxSets
        static_cast<uint32_t>(descriptorPoolSizes.size()),          // poolSizeCount
        descriptorPoolSizes.data()                                  // pPoolSizes
    };

    VK_THROW_IF_FAILED(vkCreateDescriptorPool(_device.device(), &descriptorPoolCreateInfo, nullptr, &_descriptorPool));
}


DescriptorPool::~DescriptorPool()
{
    vkDestroyDescriptorPool(_device.device(), _descriptorPool, nullptr);
}
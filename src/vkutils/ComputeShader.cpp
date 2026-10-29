#include "ComputeShader.h"

#include "util.h"

using namespace GPU;

ComputeShader::ComputeShader(
    const Device& device,
    const std::filesystem::path& shaderPath)
    : _device(device)
{
    std::vector<char> bytecode;
    loadFile(shaderPath, bytecode);

    const VkShaderModuleCreateInfo shaderModuleCreateInfo = {
        VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, nullptr, 0,        // sType, pNext, flags
        (size_t)bytecode.size(),
        reinterpret_cast<const uint32_t*>(bytecode.data())              // SPIRV
    };

    VK_THROW_IF_FAILED(vkCreateShaderModule(_device.device(), &shaderModuleCreateInfo, nullptr, &_shaderModule));
}


ComputeShader::ComputeShader(
    const Device& device,
    size_t codeSize,
    const uint32_t* pCode)
    : _device(device)
{
    const VkShaderModuleCreateInfo shaderModuleCreateInfo = {
        VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, nullptr, 0,        // sType, pNext, flags
        codeSize,
        pCode                                                           // SPIRV
    };

    VK_THROW_IF_FAILED(vkCreateShaderModule(_device.device(), &shaderModuleCreateInfo, nullptr, &_shaderModule));
}


ComputeShader::~ComputeShader()
{
    vkDeviceWaitIdle(_device.device());

    if (_shaderModule) vkDestroyShaderModule(_device.device(), _shaderModule, nullptr);

    for (VkDescriptorSetLayout& descriptorSetLayout : _descriptorSetLayouts) {
        vkDestroyDescriptorSetLayout(_device.device(), descriptorSetLayout, nullptr);
    }

    if (_descriptorPool) vkDestroyDescriptorPool(_device.device(), _descriptorPool, nullptr);

    if (_pipelineLayout) vkDestroyPipelineLayout(_device.device(), _pipelineLayout, nullptr);
}

// TODO remove
VkPipelineShaderStageCreateInfo ComputeShader::getPipelineShaderStageCreateInfo() const
{
    return {
        VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,    // sType, pNext, flags
        VK_SHADER_STAGE_COMPUTE_BIT,                                        // stage
        _shaderModule,                                                      // module
        "main",                                                             // pName
        &_specializationInfo                                                 // pSpecializationInfo
    };
}


void ComputeShader::createPipelineLayout()
{
    const VkPipelineLayoutCreateInfo pipelineLayoutCreateInfo = {
        VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, nullptr, 0,  // sType, pNext, flags
        getDescriptorSetLayoutsCount(),
        getDescriptorSetLayouts(),                                  // setLayouts
        getPushConstantRangesCount(),
        getPushConstantRanges()                                     // pushConstantRanges
    };

    VK_THROW_IF_FAILED(vkCreatePipelineLayout(_device.device(), &pipelineLayoutCreateInfo, nullptr, &_pipelineLayout));
}


void ComputeShader::createDescriptorSetLayouts(
    const std::vector<std::vector<VkDescriptorSetLayoutBinding>>& bidindings)
{
    _descriptorSetLayouts.resize(bidindings.size());

    std::vector<VkDescriptorPoolSize> descriptorPoolSizes;

    for (uint32_t iSet = 0; iSet < bidindings.size(); iSet++) {
        const VkDescriptorSetLayoutCreateInfo descriptorSetLayoutCreateInfo = {
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, nullptr, 0,    // sType, pNext, flags
            (uint32_t)bidindings[iSet].size(),
            bidindings[iSet].data()
        };

        VK_THROW_IF_FAILED(vkCreateDescriptorSetLayout(_device.device(), &descriptorSetLayoutCreateInfo, nullptr, &_descriptorSetLayouts[iSet]));

        // Keep track of all binding to allocate the descriptor pool
        for (const VkDescriptorSetLayoutBinding& binding : bidindings[iSet]) {
            descriptorPoolSizes.push_back({ binding.descriptorType, binding.descriptorCount });
        }
    }

    // DescriptorPool
    const VkDescriptorPoolCreateInfo descriptorPoolCreateInfo = {
        VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, nullptr,     // sType, pNext
        VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,          // flags
        (uint32_t)_descriptorSetLayouts.size(),                     // maxSets
        (uint32_t)descriptorPoolSizes.size(), descriptorPoolSizes.data() // descriptorPoolSizes
    };

    VK_THROW_IF_FAILED(vkCreateDescriptorPool(_device.device(), &descriptorPoolCreateInfo, nullptr, &_descriptorPool));

    // TODO: remove, shall be handled outside
    _descriptorSets.resize(bidindings.size());

    for (uint32_t iSet = 0; iSet < bidindings.size(); iSet++) {
        const VkDescriptorSetAllocateInfo descriptorSetAllocateInfo = {
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, nullptr,    // sType, pNext
            _descriptorPool,                                            // descriptorPool
            1,                                                          // descriptorSetCount
            &_descriptorSetLayouts[iSet]                                // pSetLayouts
        };

        VK_THROW_IF_FAILED(vkAllocateDescriptorSets(_device.device(), &descriptorSetAllocateInfo, &_descriptorSets[iSet]));
    }
}
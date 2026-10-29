#pragma once

#include <filesystem>
#include <vector>

#include <vkutils/Device.h>


namespace GPU
{
    class ComputeShader
    {
    public:
        ComputeShader(
            const Device& device,
            const std::filesystem::path& shaderPath);

        ComputeShader(
            const Device& device,
            size_t codeSize,
            const uint32_t* pCode);

        virtual ~ComputeShader();

        // TODO: remove
        VkPipelineShaderStageCreateInfo getPipelineShaderStageCreateInfo() const;

        const VkDescriptorSet* descriptorSets() const { return _descriptorSets.data(); }
        uint32_t descriptorSetsCount() const { return (uint32_t)_descriptorSets.size(); }

        const VkDescriptorSetLayout* getDescriptorSetLayouts() const { return _descriptorSetLayouts.data(); }
        uint32_t getDescriptorSetLayoutsCount() const { return (uint32_t)_descriptorSetLayouts.size(); }

        const VkPushConstantRange* getPushConstantRanges() const { return _pushConstantRanges.data(); }
        uint32_t getPushConstantRangesCount() const { return (uint32_t)_pushConstantRanges.size(); }

        const VkPipelineLayout pipelineLayout() const { return _pipelineLayout; }

    protected:
        void createPipelineLayout();

        void createDescriptorSetLayouts(
            const std::vector<std::vector<VkDescriptorSetLayoutBinding>>& bidindings);

        const Device& _device;

        VkShaderModule   _shaderModule   = VK_NULL_HANDLE;
        VkDescriptorPool _descriptorPool = VK_NULL_HANDLE;
        VkPipelineLayout _pipelineLayout = VK_NULL_HANDLE;

        std::vector<VkDescriptorSet> _descriptorSets;
        std::vector<VkDescriptorSetLayout> _descriptorSetLayouts;
        std::vector<VkPushConstantRange> _pushConstantRanges;

        // TODO: remove
        VkSpecializationInfo _specializationInfo = { 0 };
    };
}
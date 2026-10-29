#pragma once

#include <vkutils/Device.h>

#include <filesystem>


namespace GPU
{
    class PipelineCache
    {
    public:
        PipelineCache(
            const GPU::Device& device, const std::filesystem::path& pathCache);

        VkPipelineCache pipelineCache() const { return _pipelineCache; }

        virtual ~PipelineCache();

    private:
        const Device& _device;
        std::filesystem::path _pathCache;

        VkPipelineCache _pipelineCache = VK_NULL_HANDLE;
    };
}
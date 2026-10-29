#include "PipelineCache.h"

#include "util.h"

#include <fstream>
#include <iostream>

using namespace GPU;


PipelineCache::PipelineCache(
    const Device& device,
    const std::filesystem::path& pathCache)
    : _device(device)
    , _pathCache(pathCache)
{
    std::vector<char> cacheData;
    
    if (std::filesystem::exists(pathCache)) {
        std::ifstream fileCache(pathCache, std::ios::binary);
        fileCache.seekg(0, std::ios::end);
        cacheData.resize(fileCache.tellg());
        fileCache.seekg(0, std::ios::beg);
        fileCache.read(cacheData.data(), cacheData.size());
    }

    const VkPipelineCacheCreateInfo pipelineCacheCreateInfo = {
        VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO, nullptr, 0,   // sType, pNext, flags
        cacheData.size(), cacheData.data()                          // initialDataSize, pInitialData
    };

    VK_THROW_IF_FAILED(vkCreatePipelineCache(_device.device(), &pipelineCacheCreateInfo, nullptr, &_pipelineCache));
}


PipelineCache::~PipelineCache()
{
    // Save chache data to file
    size_t cacheSize = 0;
    std::vector<char> cacheData;

    vkGetPipelineCacheData(_device.device(), _pipelineCache, &cacheSize, nullptr);
    cacheData.resize(cacheSize);
    vkGetPipelineCacheData(_device.device(), _pipelineCache, &cacheSize, cacheData.data());

    std::ofstream fileCache(_pathCache, std::ios::binary);

    if (fileCache.is_open()) {
        fileCache.write(cacheData.data(), cacheData.size());
    }
    else {
        // Just a cerr, this is not that important and we don't want to throw an exception in the destructor
        std::cerr << "Failed to open pipeline cache file for writing: " << _pathCache << std::endl;
    }

    if (_pipelineCache != VK_NULL_HANDLE) {
        vkDestroyPipelineCache(_device.device(), _pipelineCache, nullptr);
    }
}
#include "Buffer.h"

#include "util.h"

using namespace GPU;


Buffer::Buffer(
    const Device& device,
    VkDeviceSize size,
    VkBufferUsageFlags usage,
    VkMemoryPropertyFlags memoryPropertyFlags,
    const std::vector<uint32_t>& iQueueFamilies,
    VkSharingMode sharingMode)
    : _device(device)
    , _size(size)
{
    initBuffer(usage, memoryPropertyFlags, iQueueFamilies, sharingMode);
}


Buffer::Buffer(
    const char* debugName,
    const Device& device,
    VkDeviceSize size,
    VkBufferUsageFlags usage,
    VkMemoryPropertyFlags memoryPropertyFlags,
    const std::vector<uint32_t>& iQueueFamilies,
    VkSharingMode sharingMode)
    : _device(device)
    , _size(size)
{
    initBuffer(usage, memoryPropertyFlags, iQueueFamilies, sharingMode);
    _device.setObjectName(reinterpret_cast<uint64_t>(_buffer), VK_OBJECT_TYPE_BUFFER, debugName);
}


Buffer::~Buffer()
{
    if (_bindedMemory)  unbind();
    if (_buffer)        vkDestroyBuffer(_device.device(), _buffer, nullptr);
    if (_bufferMemory)  vkFreeMemory(_device.device(), _bufferMemory, nullptr);
}


void Buffer::bind()
{
    VK_THROW_IF_FAILED(vkMapMemory(_device.device(), _bufferMemory, 0, VK_WHOLE_SIZE, 0, (void**)&_bindedMemory));
}


void Buffer::unbind()
{
    vkUnmapMemory(_device.device(), _bufferMemory);
    _bindedMemory = nullptr;
}


void Buffer::refreshDeviceToHostMemory()
{
    const VkMappedMemoryRange wholeRange = {
        VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, nullptr,     // sType, pNext
        _bufferMemory, 0, VK_WHOLE_SIZE                     // memory, offset, size
    };

    VK_THROW_IF_FAILED(vkInvalidateMappedMemoryRanges(_device.device(), 1, &wholeRange));
}


void Buffer::refreshHostToDeviceMemory()
{
    const VkMappedMemoryRange wholeRange = {
        VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, nullptr,     // sType, pNext
        _bufferMemory, 0, VK_WHOLE_SIZE                     // memory, offset, size
    };

    VK_THROW_IF_FAILED(vkFlushMappedMemoryRanges(_device.device(), 1, &wholeRange));
}


void Buffer::initBuffer(
    VkBufferUsageFlags usage,
    VkMemoryPropertyFlags memoryPropertyFlags,
    const std::vector<uint32_t>& iQueueFamilies,
    VkSharingMode sharingMode)
{
    const VkBufferCreateInfo bufferCreateInfo = {
        VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, nullptr, 0,       // sType, pNext, flags
        _size,                                                  // size
        usage,                                                  // usage
        sharingMode,                                            // sharingMode
        (uint32_t)iQueueFamilies.size(), iQueueFamilies.data()  // queueFamilyIndices
    };

    VK_THROW_IF_FAILED(vkCreateBuffer(_device.device(), &bufferCreateInfo, nullptr, &_buffer));

    VkMemoryRequirements memoryRequirements;
    vkGetBufferMemoryRequirements(_device.device(), _buffer, &memoryRequirements);

    const VkMemoryAllocateInfo memoryAllocateInfo = {
        VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, 0,
        memoryRequirements.size,
        chooseMemoryTypeFromFlags(_device.physicalDevice(), memoryRequirements, memoryPropertyFlags)
    };

    VK_THROW_IF_FAILED(vkAllocateMemory(_device.device(), &memoryAllocateInfo, nullptr, &_bufferMemory));
    VK_THROW_IF_FAILED(vkBindBufferMemory(_device.device(), _buffer, _bufferMemory, 0));
}
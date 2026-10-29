#pragma once

#include <vkutils/Device.h>


namespace GPU
{
    class Buffer
    {
    public:
        Buffer(
            const Device& device,
            VkDeviceSize size,
            VkBufferUsageFlags usage,
            VkMemoryPropertyFlags memoryPropertyFlags,
            const std::vector<uint32_t>& iQueueFamilies = {},
            VkSharingMode sharingMode = VK_SHARING_MODE_EXCLUSIVE);

        Buffer(
            const char* debugName,
            const Device& device,
            VkDeviceSize size,
            VkBufferUsageFlags usage,
            VkMemoryPropertyFlags memoryPropertyFlags,
            const std::vector<uint32_t>& iQueueFamilies = {},
            VkSharingMode sharingMode = VK_SHARING_MODE_EXCLUSIVE);

        virtual ~Buffer();

        VkBuffer buffer() const { return _buffer; }

        // VkMemoryPropertyFlags shall be set property to allow binding
        void bind();
        void unbind();
        uint8_t* mappedMemory() const { return _bindedMemory; }

        VkDeviceSize size() const { return _size; }

        void refreshDeviceToHostMemory();
        void refreshHostToDeviceMemory();

    private:
        void initBuffer(
            VkBufferUsageFlags usage,
            VkMemoryPropertyFlags memoryPropertyFlags,
            const std::vector<uint32_t>& iQueueFamilies,
            VkSharingMode sharingMode
        );

        const Device& _device;

        VkBuffer _buffer                = VK_NULL_HANDLE;
        VkDeviceMemory _bufferMemory    = VK_NULL_HANDLE;

        uint8_t* _bindedMemory = nullptr;

        VkDeviceSize _size;
    };
}
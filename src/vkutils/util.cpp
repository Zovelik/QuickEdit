#include "util.h"

#include <cstring>
#include <fstream>
#include <cassert>


void loadFile(const std::filesystem::path& path, std::vector<char>& fileContent)
{
    std::ifstream fin(path, std::ios::binary);

    if (!fin) {
        throw std::runtime_error("Could not open file for reading");
    }

    fin.seekg(0, std::ios::end);
    fileContent.resize(fin.tellg());
    fin.seekg(0, std::ios::beg);

    if (!fin.read(fileContent.data(), fileContent.size())) {
        throw std::runtime_error("Error while reading the file");
    }
}


uint32_t chooseMemoryTypeFromFlags(
    const VkPhysicalDevice physicalDevice,
    const VkMemoryRequirements& memoryRequirements,
    VkMemoryPropertyFlags requiredFlags)
{
    VkPhysicalDeviceMemoryProperties memoryProperties;
    vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memoryProperties);

    for (size_t i_bit = 0; i_bit < memoryProperties.memoryTypeCount; i_bit++) {
        if (memoryRequirements.memoryTypeBits & (1 << i_bit)) {
            if ((memoryProperties.memoryTypes[i_bit].propertyFlags & requiredFlags) == requiredFlags) {
                return static_cast<uint32_t>(i_bit);
            }
        }
    }

    // Falling back to an arbitrary type would fail later in a less obvious
    // way (e.g. mapping memory that is not host visible).
    throw std::runtime_error("[VULKAN] Could not find a suitable memory type");
}


uint16_t floatToHalf(float f)
{
    uint32_t bits;
    std::memcpy(&bits, &f, sizeof(bits));

    uint32_t sign = (bits >> 16) & 0x8000;
    uint32_t mant = bits & 0x007fffff;
    int32_t exp = ((bits >> 23) & 0xff) - 127 + 15;

    if (exp <= 0)
    {
        if (exp < -10)
            return (uint16_t)sign; // = 0

        mant |= 0x00800000;
        uint32_t t = 14 - exp;
        uint16_t m = (uint16_t)(mant >> (t + 1));
        if (mant & (1 << t)) m++; // round-nearest
        return sign | m;
    }
    else if (exp >= 0x1f)
    {
        return sign | 0x7c00; // Inf
    }
    else
    {
        uint16_t m = (uint16_t)(mant >> 13);
        if (mant & 0x1000) m++; // round
        return sign | (exp << 10) | m;
    }
}


uint32_t get_el_with_padding(uint32_t n_el, uint32_t el_per_workgroup)
{
    const uint32_t n_workgroups = (n_el + el_per_workgroup - 1) / el_per_workgroup;
    const uint32_t n_el_with_padding = n_workgroups * el_per_workgroup;

    assert(n_el <= n_el_with_padding);

    return n_el_with_padding;
}


uint32_t get_n_workgroups(uint32_t n_el, uint32_t el_per_workgroup)
{
    const uint32_t n_workgroups = (n_el + el_per_workgroup - 1) / el_per_workgroup;

    return n_workgroups;
}


uint32_t maxMipsForResolution(uint32_t width, uint32_t height)
{
    uint32_t w = width;
    uint32_t h = height;

    uint32_t maxMips = 1;

    while (w > 1 || h > 1) {
        w = std::max(1u, w >> 1);
        h = std::max(1u, h >> 1);
        maxMips++;
    }

    return maxMips;
}


void imageBarrierDstToSrcFields(VkImageMemoryBarrier& barrier)
{
    barrier.srcAccessMask = barrier.dstAccessMask;
    barrier.oldLayout = barrier.newLayout;
    barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex;
}
#include <media/HwDevice.h>

extern "C" {
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_vulkan.h>
}

#include <stdexcept>
#include <string>


namespace media
{

HwDevice::HwDevice(const VulkanDeviceDesc& desc)
    : _desc(desc)
{
    _context = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_VULKAN);

    if (!_context) {
        throw std::runtime_error("Could not allocate the FFmpeg Vulkan device");
    }

    AVHWDeviceContext* device = (AVHWDeviceContext*)_context->data;
    AVVulkanDeviceContext* vulkan = (AVVulkanDeviceContext*)device->hwctx;

    device->user_opaque = this;

    vulkan->get_proc_addr = vkGetInstanceProcAddr;
    vulkan->inst = _desc.instance;
    vulkan->phys_dev = _desc.physicalDevice;
    vulkan->act_dev = _desc.device;
    vulkan->device_features = *_desc.features;

    vulkan->enabled_inst_extensions = _desc.instanceExtensions.data();
    vulkan->nb_enabled_inst_extensions = (int)_desc.instanceExtensions.size();
    vulkan->enabled_dev_extensions = _desc.deviceExtensions.data();
    vulkan->nb_enabled_dev_extensions = (int)_desc.deviceExtensions.size();

    for (const VulkanDeviceDesc::QueueFamily& family : _desc.queueFamilies) {
        if (vulkan->nb_qf >= (int)(sizeof(vulkan->qf) / sizeof(vulkan->qf[0]))) {
            break;
        }

        AVVulkanDeviceQueueFamily& qf = vulkan->qf[vulkan->nb_qf++];
        qf.idx = (int)family.index;
        qf.num = (int)family.queueCount;
        qf.flags = (VkQueueFlagBits)family.flags;

        // Filled by FFmpeg from the device properties.
        qf.video_caps = (VkVideoCodecOperationFlagBitsKHR)0;

        if (family.flags & VK_QUEUE_VIDEO_DECODE_BIT_KHR) {
            _canDecodeVideo = true;
        }
    }

    // Deprecated in favor of VK_KHR_internally_synchronized_queues, which
    // drivers do not expose yet: the queues are shared with the application,
    // whose submissions take the same locks.
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable: 4996)
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
    vulkan->lock_queue = HwDevice::lockQueue;
    vulkan->unlock_queue = HwDevice::unlockQueue;
#if defined(_MSC_VER)
#pragma warning(pop)
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

    const int result = av_hwdevice_ctx_init(_context);

    if (result < 0) {
        av_buffer_unref(&_context);

        char error[AV_ERROR_MAX_STRING_SIZE] = {};
        av_strerror(result, error, sizeof(error));
        throw std::runtime_error(std::string("FFmpeg refused the Vulkan device: ") + error);
    }
}


HwDevice::~HwDevice()
{
    av_buffer_unref(&_context);
}


void HwDevice::lockQueue(AVHWDeviceContext* context, uint32_t family, uint32_t index)
{
    static_cast<HwDevice*>(context->user_opaque)->_desc.lockQueue(family, index, true);
}


void HwDevice::unlockQueue(AVHWDeviceContext* context, uint32_t family, uint32_t index)
{
    static_cast<HwDevice*>(context->user_opaque)->_desc.lockQueue(family, index, false);
}

}

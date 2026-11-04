#include <app/GUIBase.h>
#include <app/BorderlessWindow.h>
#include <core/Paths.h>

#include <fonts/icons.h>
#include <fonts/inter.h>
#include <fonts/IconsMaterialDesign.h>

#include <vkutils/util.h>

#include <SDL3/SDL_vulkan.h>
#include <backends/imgui_impl_sdl3.h>
#include <backends/imgui_impl_vulkan.h>
#include <imgui_internal.h>

#include <cstring>
#include <sstream>
#include <stdexcept>
#include <utility>

#include <config.h>


GUIBase::GUIBase(uint32_t width, uint32_t height)
    : _width(width), _height(height)
{
    createSDL();
    createVulkan();
    createImGui();
}


GUIBase::~GUIBase()
{
    destroyImGui();
    destroyVulkan();
    destroySDL();
}


void GUIBase::run()
{
    SDL_Event event;

    SDL_AddEventWatch(sdlEventWatch, this);

    while (!_quit) {
        // Nothing to draw while minimized: sleep until the next event instead
        // of spinning on a swapchain that cannot be acquired.
        if (SDL_GetWindowFlags(_window) & SDL_WINDOW_MINIMIZED) {
            const int wake = minimizedWakeMs();

            if (wake < 0) {
                SDL_WaitEvent(nullptr);
            }
            else {
                SDL_WaitEventTimeout(nullptr, wake);
            }
        }

        if (_requestResize) {
            onWindowResized();
            _requestResize = false;
        }

        while (SDL_PollEvent(&event)) {
            switch (event.type) {
                // The main window closed while others are open (no
                // SDL_EVENT_QUIT then).
                case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
                    if (event.window.windowID == SDL_GetWindowID(_window)) {
                        quit();
                    }
                    break;
                case SDL_EVENT_QUIT:
                    // Through quit(), which the application may override
                    // to save its work first.
                    quit();
                    break;
                // Files dragged from the file manager (but on Windows, see
                // registerFileDropTarget()).
                case SDL_EVENT_DROP_BEGIN:
                    _fileDrag = FileDrag();
                    _fileDrag.active = true;
                    break;
                case SDL_EVENT_DROP_POSITION:
                    _fileDrag.active = true;
                    _fileDrag.x = event.drop.x;
                    _fileDrag.y = event.drop.y;
                    break;
                case SDL_EVENT_DROP_FILE:
                    // SDL strings are UTF-8
                    _fileDrag.active = true;
                    _fileDrag.files.push_back(std::filesystem::u8path(event.drop.data));
                    _fileDrag.x = event.drop.x;
                    _fileDrag.y = event.drop.y;
                    break;
                case SDL_EVENT_DROP_COMPLETE:
                    if (_fileDrag.files.empty()) {
                        _fileDrag = FileDrag();
                    }
                    else {
                        _fileDrag.dropped = true;
                    }
                    break;
                case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED:
                    {
                        _displayScale = SDL_GetWindowDisplayScale(_window);
                        applyDisplayScale();
                    }
                    break;
                case SDL_EVENT_WINDOW_RESIZED:
                    _requestResize = true;
                    break;

                // Shortcuts
                case SDL_EVENT_KEY_DOWN:
                    if (event.key.mod & SDL_KMOD_CTRL) {
                        // Ctrl + O Open file dialog
                        if (event.key.key == SDLK_O) {
                            requestOpenFile();
                        }
                        // Ctrl + Shift + S Save As file
                        else if (event.key.key == SDLK_S &&
                                (event.key.mod & SDL_KMOD_SHIFT)) {
                            requestSaveAsFile();
                        }
                        // Ctrl + S Save file
                        else if (event.key.key == SDLK_S) {
                            requestSaveFile();
                        }
                    }
                    break;
            }

            eventSdl(event);
            ImGui_ImplSDL3_ProcessEvent(&event);
        }

        if (_liveResizeException) {
            std::rethrow_exception(std::exchange(_liveResizeException, nullptr));
        }

        // Trigger opening file dialog if requested
        if (_requestOpenFile) {
            _requestOpenFile = false;
            eventOpenFile(_window);
        }

        if (_requestSaveFile) {
            _requestSaveFile = false;
            eventSaveFile(_window);
        }

        if (_requestSaveAsFile) {
            _requestSaveAsFile = false;
            eventSaveAsFile(_window);
        }

        eventBeforeFrame();
        renderFrame();
    }

    SDL_RemoveEventWatch(sdlEventWatch, this);
}


void GUIBase::renderFrame()
{
    _inFrame = true;

    {
        std::scoped_lock lock(queueMutex(Queue::Graphics));
        VK_THROW_IF_FAILED(vkWaitForFences(device().device(), 1, &_fences[Queue::Graphics], VK_TRUE, UINT64_MAX));
    }

    // Acquire an image
    uint32_t iSwapchainImage = 0;
    VkSemaphore semaphoreImageAcquired = VK_NULL_HANDLE;
    VkSemaphore semaphoreTimeline = VK_NULL_HANDLE;
    uint64_t signalTimelineValue = 0;
    VkSemaphore semaphoreRenderReady = VK_NULL_HANDLE;

    VkResult acquireOk = _pSwapchain->acquireNextImage(
        &iSwapchainImage,
        &semaphoreImageAcquired,
        &semaphoreRenderReady,
        &semaphoreTimeline,
        &signalTimelineValue
    );

    if (acquireOk != VK_SUCCESS) {
        _requestResize = true;
        _inFrame = false;
        return;
    }

    {
        std::scoped_lock lock(queueMutex(Queue::Graphics));
        VK_THROW_IF_FAILED(vkResetFences(_pDevice->device(), 1, &_fences[Queue::Graphics]));
    }

    //if (_minimized) {
    //    SDL_Delay(100);
    //    continue;
    //}

    const VkCommandBufferBeginInfo commandBufferBeginInfo = {
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, nullptr,   // sType, pNext
        VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, nullptr    // flags, pInheritanceInfo
    };

    VK_THROW_IF_FAILED(vkBeginCommandBuffer(_commandBuffers[Queue::Graphics], &commandBufferBeginInfo));

    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();

    render(
        _commandBuffers[Queue::Graphics],
        _pSwapchain->image(iSwapchainImage),
        _pSwapchain->imageView(iSwapchainImage),
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
    );

    ImGui::Render();
    ImGui::UpdatePlatformWindows();

    {
        // The ImGui backend uploads its textures on the graphics queue while
        // recording, and submits and presents the windows outside the main
        // one there.
        std::scoped_lock lock(queueMutex(Queue::Graphics));
        ImGui::RenderPlatformWindowsDefault();
        cmdRenderImGui(_commandBuffers[Queue::Graphics], iSwapchainImage);
    }

    // No barrier needed: the render pass leaves the swapchain image in its
    // present layout (see createVulkanRenderPass()).

    // ------------------------------------------------------------------------
    // Submit command buffer
    // ------------------------------------------------------------------------

    VK_THROW_IF_FAILED(vkEndCommandBuffer(_commandBuffers[Queue::Graphics]));

    // The swapchain semaphores, then the ones of the frame (decoded video
    // frames, see addFrameWaitSemaphore()).
    std::vector<VkSemaphore> waitSemaphores = { semaphoreImageAcquired };
    std::vector<uint64_t> waitValues = { 0 };
    std::vector<VkPipelineStageFlags> waitStages = { VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT };

    waitSemaphores.insert(waitSemaphores.end(), _frameWaitSemaphores.begin(), _frameWaitSemaphores.end());
    waitValues.insert(waitValues.end(), _frameWaitValues.begin(), _frameWaitValues.end());
    waitStages.insert(waitStages.end(), _frameWaitStages.begin(), _frameWaitStages.end());

    std::vector<VkSemaphore> signalSemaphores = { semaphoreRenderReady, semaphoreTimeline };
    std::vector<uint64_t> signalSemaphoreValues = { 0, signalTimelineValue };

    signalSemaphores.insert(signalSemaphores.end(), _frameSignalSemaphores.begin(), _frameSignalSemaphores.end());
    signalSemaphoreValues.insert(signalSemaphoreValues.end(), _frameSignalValues.begin(), _frameSignalValues.end());

    _frameWaitSemaphores.clear();
    _frameWaitValues.clear();
    _frameWaitStages.clear();
    _frameSignalSemaphores.clear();
    _frameSignalValues.clear();

    const VkTimelineSemaphoreSubmitInfo timelineInfo = {
        VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO, nullptr,  // sType, pNext
        (uint32_t)waitValues.size(), waitValues.data(),             // pWaitSemaphoreValues
        (uint32_t)signalSemaphoreValues.size(),                     // signalSemaphoreValueCount
        signalSemaphoreValues.data(),                               // pSignalSemaphoreValues
    };

    const VkSubmitInfo submitInfo = {
        VK_STRUCTURE_TYPE_SUBMIT_INFO, &timelineInfo,       // sType, pNext
        (uint32_t)waitSemaphores.size(),                    // waitSemaphoreCount
        waitSemaphores.data(), waitStages.data(),           // waitSemaphores, pWaitDstStageMask
        1, &_commandBuffers[Queue::Graphics],               // commandBuffers
        (uint32_t)signalSemaphores.size(),                  // signalSemaphoreCount
        signalSemaphores.data()                             // signalSemaphores
    };

    {
        std::scoped_lock lock(queueMutex(Queue::Graphics));
        VK_THROW_IF_FAILED(vkQueueSubmit(_pDevice->queue(Queue::Graphics), 1, &submitInfo, _fences[Queue::Graphics]));
    }

    {
        // The queue is shared with FFmpeg, see queueMutex().
        std::scoped_lock lock(queueMutex(Queue::Graphics));
        _pSwapchain->present(_pDevice->queue(Queue::Graphics));
    }

    _inFrame = false;
}


void GUIBase::onLiveResize()
{
    if (_inFrame) {
        return;
    }

    int width, height;
    SDL_GetWindowSizeInPixels(_window, &width, &height);

    // Only redraw when the size changed: when the window is just moved, the
    // compositor keeps showing the last frame.
    if (width == _width && height == _height) {
        return;
    }

    const Uint64 now = SDL_GetTicksNS();

    if (now - _lastLiveResizeFrameNS < LiveResizeFrameIntervalNS) {
        return;
    }

    _lastLiveResizeFrameNS = now;

    onWindowResized();
    renderFrame();
}



void GUIBase::minimizeWindow()
{
    SDL_MinimizeWindow(_window);
}


void GUIBase::maximizeRestoreWindow()
{
    // Query the actual state: the OS may have changed it behind our back
    // (double-click on the caption, snapping, dragging a maximized window...)
    const bool isMaximized = (SDL_GetWindowFlags(_window) & SDL_WINDOW_MAXIMIZED) != 0;

    if (isMaximized) {
        SDL_RestoreWindow(_window);
    }
    else {
        SDL_MaximizeWindow(_window);
    }

    _isMaximized = !isMaximized;
}



void GUIBase::createSDL()
{
    // Init SDL
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::stringstream ss;
        ss << "Could not initialize SDL: " << SDL_GetError();
        throw std::runtime_error(ss.str());
    }

    SDL_SetAppMetadata(APP_NAME_STR, APP_VERSION_STR, APP_IDENTIFIER_STR);

    _window = SDL_CreateWindow(
        APP_NAME_STR,
        _width, _height,
        SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_BORDERLESS | SDL_WINDOW_HIGH_PIXEL_DENSITY
    );

    if (!_window) {
        std::stringstream ss;
        ss << "Could not create a window: " << SDL_GetError();
        throw std::runtime_error(ss.str());
    }

    SDL_SetWindowHitTest(_window, sdlHitTest, this);

    // Do not cover the taskbar when maximized.
    fixBorderlessMaximize(_window);

    // Paths of the files dragged over the window before they are dropped.
    if (!registerFileDropTarget(_window, _fileDrag)) {
        SDL_Log("File drag: SDL events only (paths known once dropped)");
    }

    SDL_SetWindowPosition(_window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);

    // Get the actual window size (may be different from requested size due to DPI scaling)
    SDL_GetWindowSizeInPixels(_window, &_width, &_height);
    _displayScale = SDL_GetWindowDisplayScale(_window);
}


void GUIBase::destroySDL()
{
    if (_window) {
        SDL_DestroyWindow(_window);
        _window = nullptr;
    }

    SDL_Quit();
}


void GUIBase::createVulkan()
{
    createVulkanInstance();
    createVulkanDevice();
    createVulkanSwapchain();
    createVulkanRenderPass();
    createVulkanFramebuffers();
    createVulkanCommandBuffers();
}


void GUIBase::destroyVulkan()
{
    destroyVulkanCommandBuffers();
    destroyVulkanFramebuffers();
    destroyVulkanRenderPass();
    destroyVulkanSwapchain();
    destroyVulkanDevice();
    destroyVulkanInstance();
}


void GUIBase::createVulkanInstance()
{
    // Query SDL for required Vulkan instance extensions
    uint32_t sdlINstsanceExtensionsCount = 0;
    const char* const* sdlInstanceExt = SDL_Vulkan_GetInstanceExtensions(&sdlINstsanceExtensionsCount);

    if (!sdlInstanceExt) {
        std::stringstream ss;
        ss << "Could not load required instance extensions: " << SDL_GetError();
        throw std::runtime_error(ss.str());
    }

    std::vector<const char*> enabledLayers;
    std::vector<const char*> instanceExtensions;

    for (uint32_t i = 0; i < sdlINstsanceExtensionsCount; i++) {
        instanceExtensions.push_back(sdlInstanceExt[i]);
    }

    // Activate validation layer in debug builds
#ifndef NDEBUG
    enabledLayers.push_back("VK_LAYER_KHRONOS_validation");
    SDL_Log("Enabling Vulkan validation layer");
    instanceExtensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
#endif // _DEBUG

    _pInstance = new GPU::Instance(enabledLayers, instanceExtensions);
    _instanceExtensions = instanceExtensions;
}


void GUIBase::destroyVulkanInstance()
{
#ifndef NDEBUG
    PFN_vkDestroyDebugUtilsMessengerEXT vkDestroyDebugUtilsMessengerEXT =
        (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(_pInstance->instance(), "vkDestroyDebugUtilsMessengerEXT");

    vkDestroyDebugUtilsMessengerEXT(_pInstance->instance(), _debugUtilsMessenger, nullptr);
#endif // _DEBUG
    SDL_Vulkan_DestroySurface(_pInstance->instance(), _surface, nullptr);

    delete _pInstance;
    _pInstance = nullptr;
}


void GUIBase::createVulkanDevice()
{
    // Redirect Vulkan validation layer messages to SDL Log
#ifndef NDEBUG
    const VkDebugUtilsMessengerCreateInfoEXT debugUtilsMessengerCreateInfo = {
        VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT, nullptr, 0,    // sType, pNext, flags
        //VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT |                     // messageSeverity
        //VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT |
        VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
        VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
        VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |                           // messageType
        VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
        VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,// |
        //VK_DEBUG_UTILS_MESSAGE_TYPE_DEVICE_ADDRESS_BINDING_BIT_EXT,
        GUIBase::debugCallback                                                  // pfnUserCallback
    };

    PFN_vkCreateDebugUtilsMessengerEXT vkCreateDebugUtilsMessengerEXT =
        (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(_pInstance->instance(), "vkCreateDebugUtilsMessengerEXT");

    VK_THROW_IF_FAILED(vkCreateDebugUtilsMessengerEXT(_pInstance->instance(), &debugUtilsMessengerCreateInfo, nullptr, &_debugUtilsMessenger));
    SDL_Log("Vulkan validation layers activated");
#endif // _DEBUG

    // Create Vulkan surface for SDL window
    if (!SDL_Vulkan_CreateSurface(_window, _pInstance->instance(), nullptr, &_surface)) {
        std::stringstream ss;
        ss << "Could not create a Vulkan surface: " << SDL_GetError();
        throw std::runtime_error(ss.str());
    }

    // Find suitable physical device and create logical device
    std::vector<const char*> deviceExtensions = {
        VK_KHR_SWAPCHAIN_EXTENSION_NAME,
        VK_KHR_16BIT_STORAGE_EXTENSION_NAME
    };

    std::vector<VkQueueFlags> queueFlags(Queue::Count);
    // The transfer queue is taken from the graphics family too: images are
    // shared between both queues without queue family ownership transfers.
    // It may be the very same VkQueue on GPUs exposing a single queue, see
    // queueMutex().
    queueFlags[Queue::Graphics] = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT;
    queueFlags[Queue::Transfer] = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT;

    // Prefer a device decoding and encoding video (Vulkan Video), each with
    // its own queue, see videoDecodeQueue() and videoEncodeQueue(). Without
    // them, videos are decoded by the CPU and encoded by other means.
    struct VideoConfiguration
    {
        bool decode;
        bool encode;
    };

    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;

    for (const VideoConfiguration configuration : { VideoConfiguration{ true, true }, VideoConfiguration{ true, false }, VideoConfiguration{ false, false } }) {
        std::vector<const char*> extensions = deviceExtensions;
        std::vector<VkQueueFlags> flags = queueFlags;

        if (configuration.decode) {
            extensions.push_back(VK_KHR_VIDEO_QUEUE_EXTENSION_NAME);
            extensions.push_back(VK_KHR_VIDEO_DECODE_QUEUE_EXTENSION_NAME);
            flags.push_back(VK_QUEUE_VIDEO_DECODE_BIT_KHR);
        }

        if (configuration.encode) {
            // FFmpeg encodes from images that are not tied to a video
            // profile, which needs maintenance1.
            extensions.push_back(VK_KHR_VIDEO_ENCODE_QUEUE_EXTENSION_NAME);
            extensions.push_back(VK_KHR_VIDEO_MAINTENANCE_1_EXTENSION_NAME);
            flags.push_back(VK_QUEUE_VIDEO_ENCODE_BIT_KHR);
        }

        physicalDevice = _pInstance->findDevice(flags, extensions, _surface);

        if (physicalDevice != VK_NULL_HANDLE) {
            _hasVideoDecodeQueue = configuration.decode;
            _hasVideoEncodeQueue = configuration.encode;
            deviceExtensions = extensions;
            queueFlags = flags;
            break;
        }
    }

    if (physicalDevice != VK_NULL_HANDLE && _hasVideoDecodeQueue) {
        // Codecs, as supported by the device.
        static const char* const codecExtensions[] = {
            VK_KHR_VIDEO_DECODE_H264_EXTENSION_NAME,
            VK_KHR_VIDEO_DECODE_H265_EXTENSION_NAME,
            VK_KHR_VIDEO_DECODE_AV1_EXTENSION_NAME,
#ifdef VK_KHR_VIDEO_DECODE_VP9_EXTENSION_NAME
            VK_KHR_VIDEO_DECODE_VP9_EXTENSION_NAME,     // Vulkan SDK 1.4.317+
#endif
            VK_KHR_VIDEO_ENCODE_H264_EXTENSION_NAME,
            VK_KHR_VIDEO_ENCODE_H265_EXTENSION_NAME,
            VK_KHR_VIDEO_ENCODE_AV1_EXTENSION_NAME,
        };

        for (const char* extension : codecExtensions) {
            const bool encode = std::strstr(extension, "encode") != nullptr;

            if ((!encode || _hasVideoEncodeQueue) && GPU::Device::deviceSupportsExtensions(physicalDevice, { extension })) {
                deviceExtensions.push_back(extension);
            }
        }
    }

    if (physicalDevice == VK_NULL_HANDLE) {
        std::stringstream ss;
        ss << "Could not find a suitable Vulkan device";
        throw std::runtime_error(ss.str());
    }

    // A small log to display the selected device
    VkPhysicalDeviceProperties physicalDeviceProperties;
    vkGetPhysicalDeviceProperties(physicalDevice, &physicalDeviceProperties);
    SDL_Log("Selected device: %s", physicalDeviceProperties.deviceName);
    _gpuName = physicalDeviceProperties.deviceName;

    SDL_Log("Vulkan Video decoding: %s, encoding: %s", _hasVideoDecodeQueue ? "yes" : "no", _hasVideoEncodeQueue ? "yes" : "no");

    // The features stay allocated: FFmpeg reads them, see deviceFeatures().
    _deviceFeatures = GUIBase::allocRequiredFeatures(_hasVideoEncodeQueue);
    keepSupportedFeatures(physicalDevice, _deviceFeatures);
    _pDevice = new GPU::Device(_pInstance->instance(), physicalDevice, queueFlags, deviceExtensions, _deviceFeatures);
    _deviceExtensions = deviceExtensions;

    for (uint32_t i = 0; i < (uint32_t)queueFlags.size(); i++) {
        if (!_queueMutexes.count(_pDevice->queue(i))) {
            _queueMutexes[_pDevice->queue(i)] = std::make_unique<std::mutex>();
        }
    }

    // With the caches of the system, not in the current folder (often the
    // one of the project opened).
    _pPipelineCache = new GPU::PipelineCache(*_pDevice, core::cacheDirectory() / "pipeline_cache.bin");
}


std::mutex& GUIBase::queueMutex(Queue queue)
{
    return queueMutex(_pDevice->queue(queue));
}


std::mutex& GUIBase::queueMutex(VkQueue queue)
{
    // Created with the device, never modified after: no lock needed.
    return *_queueMutexes.at(queue);
}


void GUIBase::addFrameWaitSemaphore(VkSemaphore semaphore, uint64_t value, VkPipelineStageFlags stage)
{
    _frameWaitSemaphores.push_back(semaphore);
    _frameWaitValues.push_back(value);
    _frameWaitStages.push_back(stage);
}


void GUIBase::addFrameSignalSemaphore(VkSemaphore semaphore, uint64_t value)
{
    _frameSignalSemaphores.push_back(semaphore);
    _frameSignalValues.push_back(value);
}


void GUIBase::destroyVulkanDevice()
{
    delete _pPipelineCache;
    _pPipelineCache = nullptr;

    _queueMutexes.clear();

    delete _pDevice;
    _pDevice = nullptr;

    freeRequiredFeatures(_deviceFeatures);
    _deviceFeatures = nullptr;
}


void GUIBase::createVulkanSwapchain()
{
    _pSwapchain = new GPU::Swapchain(*_pDevice, _surface, _width, _height, true);
}


void GUIBase::destroyVulkanSwapchain()
{
    delete _pSwapchain;
    _pSwapchain = nullptr;
}


void GUIBase::createVulkanCommandBuffers()
{
    // Init Vulkan resources
    for (uint32_t iQueue = 0; iQueue < Queue::Count; iQueue++) {
        const VkCommandPoolCreateInfo commandPoolCreateInfo = {
            VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, nullptr,
            VK_COMMAND_POOL_CREATE_TRANSIENT_BIT | VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
            device().queueFamilyIdx(iQueue)
        };

        VK_THROW_IF_FAILED(vkCreateCommandPool(device().device(), &commandPoolCreateInfo, nullptr, &_commandPools[iQueue]));

        const VkCommandBufferAllocateInfo commandBufferAllocateInfo = {
            VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, nullptr,
            _commandPools[iQueue],
            VK_COMMAND_BUFFER_LEVEL_PRIMARY,
            1
        };

        VK_THROW_IF_FAILED(vkAllocateCommandBuffers(device().device(), &commandBufferAllocateInfo, &_commandBuffers[iQueue]));

        // Synchronization for command buffers
        const VkFenceCreateInfo fenceCreateInfo = {
            VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, nullptr,               // sType, pNext
            VK_FENCE_CREATE_SIGNALED_BIT                                // flags
        };

        VK_THROW_IF_FAILED(vkCreateFence(device().device(), &fenceCreateInfo, nullptr, &_fences[iQueue]));
    }
}


void GUIBase::destroyVulkanCommandBuffers()
{
    VK_THROW_IF_FAILED(vkWaitForFences(_pDevice->device(), _fences.size(), _fences.data(), VK_TRUE, UINT64_MAX));

    for (uint32_t iQueue = 0; iQueue < Queue::Count; iQueue++) {
        std::scoped_lock lock(queueMutex((Queue)iQueue));
        VK_THROW_IF_FAILED(vkQueueWaitIdle(_pDevice->queue(iQueue)));
    }

    for (uint32_t iQueue = 0; iQueue < Queue::Count; iQueue++) {
        vkDestroyFence(_pDevice->device(), _fences[iQueue], nullptr);
        vkDestroyCommandPool(_pDevice->device(), _commandPools[iQueue], nullptr);
    }
}


void GUIBase::createVulkanRenderPass()
{
    const VkAttachmentDescription attachmentDescription = {
        0,                                              // flags
        _pSwapchain->format(),                          // format
        VK_SAMPLE_COUNT_1_BIT,                          // samples
        VK_ATTACHMENT_LOAD_OP_CLEAR,                    // loadOp
        VK_ATTACHMENT_STORE_OP_STORE,                   // storeOp
        VK_ATTACHMENT_LOAD_OP_DONT_CARE,                // stencilLoadOp
        VK_ATTACHMENT_STORE_OP_DONT_CARE,               // stencilStoreOp
        VK_IMAGE_LAYOUT_UNDEFINED,                      // initialLayout
        _pSwapchain->optimalLayout(),                   // finalLayout
    };

    const VkAttachmentReference colorReference = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };

    const VkSubpassDescription subpassDescription = {
        0,                                              // flags
        VK_PIPELINE_BIND_POINT_GRAPHICS,                // pipelineBindPoint
        0, nullptr,                                     // pInputAttachments
        1, &colorReference,                             // pColorAttachments
        nullptr,                                        // pResolveAttachments
        nullptr,                                        // pDepthStencilAttachment
        0, nullptr                                      // preserveAttachmentCount
    };

    const VkSubpassDependency subpassDepency = {
        VK_SUBPASS_EXTERNAL, 0,                         // srcSubpass, dstSubpass
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,  // srcStageMask
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,  // dstStageMask
        VK_ACCESS_COLOR_ATTACHMENT_READ_BIT,            // srcAccessMask
        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,           // dstAccessMask
        0                                               // dependencyFlags
    };

    const VkRenderPassCreateInfo renderPassCreateInfo = {
        VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, nullptr, 0,      // sType, pNext, flags
        1, &attachmentDescription,                      // attachments
        1, &subpassDescription,                         // subpasses
        1, &subpassDepency                              // dependencies
    };

    VK_THROW_IF_FAILED(vkCreateRenderPass(_pDevice->device(), &renderPassCreateInfo, nullptr, &_renderPass));
}


void GUIBase::destroyVulkanRenderPass()
{
    vkDestroyRenderPass(_pDevice->device(), _renderPass, nullptr);
    _renderPass = VK_NULL_HANDLE;
}


void GUIBase::createVulkanFramebuffers()
{
    _framebuffers.resize(_pSwapchain->nSwapchainImages());

    for (uint32_t i = 0; i < _pSwapchain->nSwapchainImages(); i++) {
        const VkImageView attachments[] = {
            _pSwapchain->imageView(i)
        };

        const VkFramebufferCreateInfo framebufferCreateInfo = {
            VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, nullptr, 0,  // sType, pNext, flags
            _renderPass,                                            // renderPass
            C_ARRAY_SIZE(attachments), attachments,                 // attachmentCount, pAttachments
            _pSwapchain->width(), _pSwapchain->height(), 1          // width, height, layers
        };

        VK_THROW_IF_FAILED(vkCreateFramebuffer(_pDevice->device(), &framebufferCreateInfo, nullptr, &_framebuffers[i]));
    }
}


void GUIBase::destroyVulkanFramebuffers()
{
    for (uint32_t i = 0; i < _framebuffers.size(); i++) {
        vkDestroyFramebuffer(_pDevice->device(), _framebuffers[i], nullptr);
    }

    _framebuffers.clear();
}


void GUIBase::createImGui()
{
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    ImGuiIO& io = ImGui::GetIO();

    // The layout of the windows with the other settings of the user, not
    // next to the executable (not writable once installed).
    static const std::string iniPath = (core::configDirectory() / "imgui.ini").string();
    io.IniFilename = iniPath.c_str();

    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;     // Enable Keyboard Controls
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;      // Enable Gamepad Controls
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;         // Enable Docking
    io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;        // Windows can leave the main one

    // Setup Dear ImGui style
    ImGui::StyleColorsDark();
    //ImGui::StyleColorsLight();
    ImGui_ImplSDL3_InitForVulkan(_window);

    ImGuiStyle& style = ImGui::GetStyle();
    style.FontSizeBase = 16.f;

    style.WindowRounding = 0.0f;
    style.Colors[ImGuiCol_WindowBg].w = 1.0f;

    resizeImGui();

    // Inter has its own glyphs in the Private Use Area (U+E000-U+E2DC) which
    // would take precedence over the merged Material icons: leave that area
    // to the icon font.
    static const ImWchar privateUseArea[] = { 0xE000, 0xF8FF, 0 };

    ImFontConfig interConfig;
    interConfig.GlyphExcludeRanges = privateUseArea;

    ImFont* font = io.Fonts->AddFontFromMemoryCompressedTTF(inter_compressed_data, inter_compressed_size, 0.0f, &interConfig);

    {
        ImGuiStyle* style = &ImGui::GetStyle();

        style->WindowPadding = ImVec2(8, 8);
        style->WindowRounding = 5.0f;
        style->FramePadding = ImVec2(5, 5);
        style->FrameRounding = 4.0f;
        style->ItemSpacing = ImVec2(12, 8);
        style->ItemInnerSpacing = ImVec2(8, 6);
        style->IndentSpacing = 25.0f;
        style->ScrollbarSize = 15.0f;
        style->ScrollbarRounding = 9.0f;
        style->GrabMinSize = 5.0f;
        style->GrabRounding = 3.0f;

        // Dark theme with sober blue accents.
        const ImVec4 accent        = ImVec4(0.20f, 0.48f, 0.82f, 1.00f);
        const ImVec4 accentHovered = ImVec4(0.28f, 0.56f, 0.90f, 1.00f);
        const ImVec4 accentActive  = ImVec4(0.16f, 0.40f, 0.72f, 1.00f);

        style->Colors[ImGuiCol_Text] = ImVec4(0.86f, 0.86f, 0.86f, 1.00f);
        style->Colors[ImGuiCol_TextDisabled] = ImVec4(0.50f, 0.50f, 0.50f, 1.00f);
        style->Colors[ImGuiCol_WindowBg] = ImVec4(0.13f, 0.13f, 0.13f, 1.00f);
        style->Colors[ImGuiCol_ChildBg] = ImVec4(0.15f, 0.15f, 0.15f, 0.60f);
        style->Colors[ImGuiCol_PopupBg] = ImVec4(0.11f, 0.11f, 0.11f, 0.98f);
        style->Colors[ImGuiCol_Border] = ImVec4(0.28f, 0.28f, 0.28f, 0.55f);
        style->Colors[ImGuiCol_BorderShadow] = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
        style->Colors[ImGuiCol_FrameBg] = ImVec4(0.20f, 0.20f, 0.20f, 1.00f);
        style->Colors[ImGuiCol_FrameBgHovered] = ImVec4(0.27f, 0.27f, 0.27f, 1.00f);
        style->Colors[ImGuiCol_FrameBgActive] = ImVec4(accent.x, accent.y, accent.z, 0.55f);
        style->Colors[ImGuiCol_TitleBg] = ImVec4(0.10f, 0.10f, 0.10f, 1.00f);
        style->Colors[ImGuiCol_TitleBgCollapsed] = ImVec4(0.10f, 0.10f, 0.10f, 0.75f);
        style->Colors[ImGuiCol_TitleBgActive] = ImVec4(0.16f, 0.16f, 0.16f, 1.00f);
        style->Colors[ImGuiCol_MenuBarBg] = ImVec4(0.15f, 0.15f, 0.15f, 1.00f);
        style->Colors[ImGuiCol_ScrollbarBg] = ImVec4(0.11f, 0.11f, 0.11f, 1.00f);
        style->Colors[ImGuiCol_ScrollbarGrab] = ImVec4(0.30f, 0.30f, 0.30f, 1.00f);
        style->Colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.38f, 0.38f, 0.38f, 1.00f);
        style->Colors[ImGuiCol_ScrollbarGrabActive] = accent;
        //style->Colors[ImGuiCol_ComboBg] = ImVec4(0.20f, 0.20f, 0.20f, 1.00f);
        style->Colors[ImGuiCol_CheckMark] = accentHovered;
        style->Colors[ImGuiCol_SliderGrab] = accent;
        style->Colors[ImGuiCol_SliderGrabActive] = accentHovered;
        // Neutral buttons (and combo arrows); the accent for the pressed and
        // chosen ones.
        style->Colors[ImGuiCol_Button] = ImVec4(0.25f, 0.25f, 0.25f, 1.00f);
        style->Colors[ImGuiCol_ButtonHovered] = ImVec4(0.32f, 0.32f, 0.32f, 1.00f);
        style->Colors[ImGuiCol_ButtonActive] = accent;
        style->Colors[ImGuiCol_Header] = ImVec4(accent.x, accent.y, accent.z, 0.55f);
        style->Colors[ImGuiCol_HeaderHovered] = ImVec4(accent.x, accent.y, accent.z, 0.80f);
        style->Colors[ImGuiCol_HeaderActive] = accent;
        //style->Colors[ImGuiCol_Column] = ImVec4(0.28f, 0.28f, 0.28f, 0.55f);
        //style->Colors[ImGuiCol_ColumnHovered] = ImVec4(accent.x, accent.y, accent.z, 0.78f);
        //style->Colors[ImGuiCol_ColumnActive] = accent;
        style->Colors[ImGuiCol_Separator] = ImVec4(0.28f, 0.28f, 0.28f, 0.55f);
        style->Colors[ImGuiCol_SeparatorHovered] = ImVec4(accent.x, accent.y, accent.z, 0.78f);
        style->Colors[ImGuiCol_SeparatorActive] = accent;
        style->Colors[ImGuiCol_ResizeGrip] = ImVec4(0.28f, 0.28f, 0.28f, 0.25f);
        style->Colors[ImGuiCol_ResizeGripHovered] = ImVec4(accent.x, accent.y, accent.z, 0.78f);
        style->Colors[ImGuiCol_ResizeGripActive] = accent;
        style->Colors[ImGuiCol_Tab] = ImVec4(0.16f, 0.16f, 0.16f, 1.00f);
        style->Colors[ImGuiCol_TabHovered] = ImVec4(accent.x, accent.y, accent.z, 0.80f);
        style->Colors[ImGuiCol_TabActive] = ImVec4(accent.x, accent.y, accent.z, 0.65f);
        style->Colors[ImGuiCol_TabUnfocused] = ImVec4(0.13f, 0.13f, 0.13f, 1.00f);
        style->Colors[ImGuiCol_TabUnfocusedActive] = ImVec4(0.18f, 0.18f, 0.18f, 1.00f);
        style->Colors[ImGuiCol_DockingPreview] = ImVec4(accent.x, accent.y, accent.z, 0.70f);
        style->Colors[ImGuiCol_DockingEmptyBg] = ImVec4(0.10f, 0.10f, 0.10f, 1.00f);
        style->Colors[ImGuiCol_PlotLines] = ImVec4(0.61f, 0.61f, 0.61f, 1.00f);
        style->Colors[ImGuiCol_PlotLinesHovered] = accentHovered;
        style->Colors[ImGuiCol_PlotHistogram] = ImVec4(0.61f, 0.61f, 0.61f, 1.00f);
        style->Colors[ImGuiCol_PlotHistogramHovered] = accentHovered;
        style->Colors[ImGuiCol_TextSelectedBg] = ImVec4(accent.x, accent.y, accent.z, 0.43f);
        style->Colors[ImGuiCol_NavHighlight] = accentHovered;
        style->Colors[ImGuiCol_ModalWindowDimBg] = ImVec4(0.05f, 0.05f, 0.05f, 0.60f);

        // The windows taken out of the main one are windows of the system:
        // square, as theirs.
        style->WindowRounding = 0.0f;
    }

    _baseStyle = ImGui::GetStyle();
    applyDisplayScale();
}


void GUIBase::applyDisplayScale()
{
    ImGuiStyle& style = ImGui::GetStyle();

    style = _baseStyle;
    style.ScaleAllSizes(_displayScale);
    style.FontScaleDpi = _displayScale;
}


void GUIBase::resizeImGui()
{
    ImGuiIO& io = ImGui::GetIO();

    if (io.BackendRendererUserData != nullptr) {
        ImGui_ImplVulkan_Shutdown();
    }

    VkFormat swapchainFormat = _pSwapchain->format();

    ImGui_ImplVulkan_InitInfo initInfo{
        GPU::Instance::API_VERSION,                     // ApiVersion
        _pInstance->instance(),                         // Instance
        _pDevice->physicalDevice(),                     // PhysicalDevice
        _pDevice->device(),                             // Device
        _pDevice->queueFamilyIdx(Queue::Graphics),      // QueueFamily
        _pDevice->queue(Queue::Graphics),               // Queue
        VK_NULL_HANDLE,                                 // DescriptorPool
        1024,                                           // DescriptorPoolSize
        _pSwapchain->minImages(),                       // MinImageCount
        _pSwapchain->nSwapchainImages(),                // ImageCount
        _pPipelineCache->pipelineCache(),               // PipelineCache
        { // PipelineInfoMain
            _renderPass,                                // RenderPass
            0,                                          // Subpass
            VK_SAMPLE_COUNT_1_BIT                       // MSAASamples
        }
    };

    ImGui_ImplVulkan_Init(&initInfo);
}


void GUIBase::cmdRenderImGui(VkCommandBuffer commandBuffer, uint32_t imageIndex)
{
    /*
    ImDrawData* draw_data = ImGui::GetDrawData();

    const VkRenderingAttachmentInfo colorAttachmentInfo = {
        VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, nullptr,   // sType, pNext
        targetImageView,                                        // imageView
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,               // imageLayout

        VK_RESOLVE_MODE_NONE,                                   // resolveMode
        VK_NULL_HANDLE,                                         // resolveImageView
        VK_IMAGE_LAYOUT_UNDEFINED,                              // resolveImageLayout

        VK_ATTACHMENT_LOAD_OP_LOAD,                             // loadOp
        VK_ATTACHMENT_STORE_OP_STORE,                           // storeOp

        {0.f, 0.f, 0.f, 1.f}
    };

    const VkRenderingInfo renderingInfo = {
        VK_STRUCTURE_TYPE_RENDERING_INFO, nullptr, 0,           // sType, pNext, flags
        {                                                       // renderArea
            {0, 0},                                             // offset
            {
                _width,
                _height
            },                                                  // extent
        },
        1, 0,                                                   // layerCount, viewMask
        1, &colorAttachmentInfo,                                // colorAttachmentCount, pColorAttachments
        nullptr,                                                // pDepthAttachment
        nullptr                                                 // pStencilAttachment
    };

    vkCmdBeginRendering(commandBuffer, &renderingInfo);
    ImGui_ImplVulkan_RenderDrawData(draw_data, commandBuffer);
    vkCmdEndRendering(commandBuffer);
    */
    const VkClearValue clear = {0.f, 0.f, 0.f, 1.f };

    const VkRenderPassBeginInfo renderPassBeginInfo = {
        VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, nullptr,  // sType, pNext
        _renderPass,                                        // renderPass
        _framebuffers[imageIndex],                          // framebuffer
        {                                                   // renderArea
            {0, 0},                                         // offset
            {
                _pSwapchain->width(),
                _pSwapchain->height()
            },                                              // extent
        },
        1, &clear                                           // clearValueCount, pClearValues
    };

    vkCmdBeginRenderPass(commandBuffer, &renderPassBeginInfo, VK_SUBPASS_CONTENTS_INLINE);
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), commandBuffer);
    vkCmdEndRenderPass(commandBuffer);
}


void GUIBase::destroyImGui()
{
    vkWaitForFences(_pDevice->device(), 1, &_fences[Queue::Graphics], VK_TRUE, UINT64_MAX);
    ImGui_ImplVulkan_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
}


void GUIBase::onWindowResized()
{
    int width, height;
    SDL_GetWindowSizeInPixels(_window, &width, &height);
    const bool minimized = SDL_GetWindowFlags(_window) & SDL_WINDOW_MINIMIZED;

    // Check if a resize is necessary
    if (width > 0 && height > 0 && !minimized) {
        VK_THROW_IF_FAILED(vkWaitForFences(_pDevice->device(), 1, &_fences[Queue::Graphics], VK_TRUE, UINT64_MAX));

        {
            std::scoped_lock lock(queueMutex(Queue::Graphics));
            VK_THROW_IF_FAILED(vkQueueWaitIdle(_pDevice->queue(Queue::Graphics)));
        }

        _pSwapchain->updateSwapchain(
            width, height,
            true
        );

        _width  = _pSwapchain->width();
        _height = _pSwapchain->height();

        destroyVulkanFramebuffers();
        createVulkanFramebuffers();

        ImGui_ImplVulkan_SetMinImageCount(_pSwapchain->minImages());
    }
}


#ifndef NDEBUG
VKAPI_ATTR VkBool32 VKAPI_CALL GUIBase::debugCallback(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT type,
    const VkDebugUtilsMessengerCallbackDataEXT* callbackData,
    void* userData)
{
    switch (severity) {
    case VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT:
        SDL_LogVerbose(0, callbackData->pMessage);
        break;
    case VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT:
        SDL_LogInfo(0, callbackData->pMessage);
        break;
    case VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT:
        SDL_LogWarn(0, callbackData->pMessage);
        break;
    case VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT:
        SDL_LogError(0, callbackData->pMessage);
        break;
    case VK_DEBUG_UTILS_MESSAGE_SEVERITY_FLAG_BITS_MAX_ENUM_EXT:
        break;
    }

    return VK_FALSE;
}
#endif // _DEBUG


#define VK_APPEND_FEATURE(_Type, _Var, _Prev) \
    _Type* _Var = (_Type*)std::malloc(sizeof(_Type)); \
    std::memset(_Var, 0, sizeof(_Type)); \
    _Prev->pNext = (void*)_Var; \
    _Prev = (VkPhysicalDeviceFeatures2*)_Var;


void GUIBase::keepSupportedFeatures(VkPhysicalDevice physicalDevice, void* features)
{
    // The structures of the chain: a header, then only VkBool32 fields.
    auto sizeOf = [](VkStructureType type) -> size_t {
        switch (type) {
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2:                     return sizeof(VkPhysicalDeviceFeatures2);
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES:            return sizeof(VkPhysicalDeviceVulkan11Features);
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES:            return sizeof(VkPhysicalDeviceVulkan12Features);
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES:            return sizeof(VkPhysicalDeviceVulkan13Features);
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VIDEO_MAINTENANCE_1_FEATURES_KHR: return sizeof(VkPhysicalDeviceVideoMaintenance1FeaturesKHR);
            default:                                                                return 0;
        }
    };

    // The same chain, filled by the device.
    std::vector<std::vector<uint8_t>> supported;

    for (const VkBaseOutStructure* s = (const VkBaseOutStructure*)features; s; s = s->pNext) {
        std::vector<uint8_t>& copy = supported.emplace_back(std::max(sizeOf(s->sType), sizeof(VkBaseOutStructure)), (uint8_t)0);
        ((VkBaseOutStructure*)copy.data())->sType = s->sType;
    }

    for (size_t i = 0; i + 1 < supported.size(); i++) {
        ((VkBaseOutStructure*)supported[i].data())->pNext = (VkBaseOutStructure*)supported[i + 1].data();
    }

    if (supported.empty()) {
        return;
    }

    vkGetPhysicalDeviceFeatures2(physicalDevice, (VkPhysicalDeviceFeatures2*)supported.front().data());

    size_t index = 0;

    for (VkBaseOutStructure* s = (VkBaseOutStructure*)features; s; s = s->pNext, index++) {
        const size_t count = (sizeOf(s->sType) > sizeof(VkBaseOutStructure)) ? (sizeOf(s->sType) - sizeof(VkBaseOutStructure)) / sizeof(VkBool32) : 0;
        VkBool32* wanted = (VkBool32*)((uint8_t*)s + sizeof(VkBaseOutStructure));
        const VkBool32* available = (const VkBool32*)(supported[index].data() + sizeof(VkBaseOutStructure));

        for (size_t field = 0; field < count; field++) {
            if (wanted[field] && !available[field]) {
                SDL_Log("Vulkan: feature %zu of structure %d not supported by the device, not asked for", field, (int)s->sType);
                wanted[field] = VK_FALSE;
            }
        }
    }
}


void* GUIBase::allocRequiredFeatures(bool videoMaintenance1)
{
    VkPhysicalDeviceFeatures2* lastFeaturePtr = nullptr;

    VkPhysicalDeviceFeatures2* features = (VkPhysicalDeviceFeatures2*)std::malloc(sizeof(VkPhysicalDeviceFeatures2));
    std::memset(features, 0, sizeof(VkPhysicalDeviceFeatures2));
    lastFeaturePtr = features;

    features->sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;

    features->features.multiDrawIndirect = VK_TRUE;
    features->features.samplerAnisotropy = VK_TRUE;
    features->features.geometryShader = VK_TRUE;
    features->features.shaderStorageImageExtendedFormats = VK_TRUE;   // r8 / rg8 storage (video planes)

    //VkPhysicalDeviceVulkan11Features* vulkan11Features;
    VK_APPEND_FEATURE(VkPhysicalDeviceVulkan11Features, vulkan11Features, lastFeaturePtr);
    vulkan11Features->sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
    vulkan11Features->shaderDrawParameters = VK_TRUE;
    vulkan11Features->storageBuffer16BitAccess = VK_TRUE;
    vulkan11Features->samplerYcbcrConversion = VK_TRUE;      // FFmpeg Vulkan

    VK_APPEND_FEATURE(VkPhysicalDeviceVulkan12Features, vulkan12Features, lastFeaturePtr);
    vulkan12Features->sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    vulkan12Features->shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
    vulkan12Features->runtimeDescriptorArray = VK_TRUE;
    vulkan12Features->timelineSemaphore = VK_TRUE;
    vulkan12Features->shaderFloat16 = VK_TRUE;
    vulkan12Features->shaderStorageBufferArrayNonUniformIndexing = VK_TRUE;
    vulkan12Features->vulkanMemoryModel = VK_TRUE;
    vulkan12Features->vulkanMemoryModelDeviceScope = VK_TRUE;
    vulkan12Features->hostQueryReset = VK_TRUE;              // FFmpeg Vulkan Video

    // Dynamic rendering
    VK_APPEND_FEATURE(VkPhysicalDeviceVulkan13Features, vulkan13Features, lastFeaturePtr);
    vulkan13Features->sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    vulkan13Features->dynamicRendering = VK_TRUE;
    vulkan13Features->synchronization2 = VK_TRUE;            // FFmpeg Vulkan

    if (videoMaintenance1) {
        VK_APPEND_FEATURE(VkPhysicalDeviceVideoMaintenance1FeaturesKHR, maintenance1Features, lastFeaturePtr);
        maintenance1Features->sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VIDEO_MAINTENANCE_1_FEATURES_KHR;
        maintenance1Features->videoMaintenance1 = VK_TRUE;
    }

    return features;
}


void GUIBase::freeRequiredFeatures(void* features)
{
    if (features) {
        void* nextFeature = ((VkPhysicalDeviceFeatures2*)features)->pNext;

        if (nextFeature) {
            freeRequiredFeatures(nextFeature);
        }

        std::free(features);
        features = NULL;
    }
}


bool SDLCALL GUIBase::sdlEventWatch(void* userdata, SDL_Event* event)
{
    // data1 is 1 for the expose events sent during a live resize.
    GUIBase* gui = static_cast<GUIBase*>(userdata);

    if (event->type == SDL_EVENT_WINDOW_EXPOSED && event->window.data1 == 1 && !gui->_liveResizeException) {
        try {
            gui->onLiveResize();
        }
        catch (...) {
            gui->_liveResizeException = std::current_exception();
            gui->_inFrame = false;
        }
    }

    return true;
}


bool GUIBase::windowOverTitlebar(float x, float y) const
{
    ImGuiContext* context = ImGui::GetCurrentContext();

    if (!context || _titlebarWindowId == 0) {
        return false;
    }

    // A window being moved by ImGui keeps the mouse, wherever it is.
    if (context->MovingWindow) {
        return true;
    }

    const ImGuiViewport* mainViewport = ImGui::GetMainViewport();
    const ImVec2 point(mainViewport->Pos.x + x, mainViewport->Pos.y + y);

    // Windows are in display order, back to front. The title bar window is
    // the background, so any other one under the point is in front of it,
    // in the main viewport or in one of its own (a platform window over the
    // main one, which may get the mouse while ImGui moves it).
    for (int i = context->Windows.Size - 1; i >= 0; --i) {
        const ImGuiWindow* window = context->Windows[i];

        if (!window->WasActive || window->Hidden ||
            (window->Flags & (ImGuiWindowFlags_ChildWindow | ImGuiWindowFlags_Tooltip))) {
            continue;
        }

        const ImGuiWindow* root = window->RootWindowDockTree ? window->RootWindowDockTree : window;

        if (root->ID != _titlebarWindowId && window->Rect().Contains(point)) {
            return true;
        }
    }

    return false;
}


SDL_HitTestResult SDLCALL GUIBase::sdlHitTest(SDL_Window* win, const SDL_Point* area, void* data)
{
    GUIBase* obj = (GUIBase*)data;
    assert(win == obj->_window);

    int width, height;
    SDL_GetWindowSize(obj->_window, &width, &height);

    const int borderX = (int)(8.f * obj->_displayScale); // Horizontal resize border thickness in pixels
    const int borderY = (int)(8.f * obj->_displayScale); // Vertical resize border thickness in pixels

    int x = area->x;
    int y = area->y;

    // Title bar buttons and menus must always receive clicks, never resize/drag,
    // as the floating windows moved over the title bar.
    if (y < obj->titlebarHeight()) {
        const bool inButtons = x >= (width - obj->totalButtonWidth());
        const bool inClient  = x >= obj->_titlebarClientMinX && x < obj->_titlebarClientMaxX;

        if (inButtons || inClient || obj->windowOverTitlebar((float)x, (float)y)) {
            return SDL_HITTEST_NORMAL;
        }
    }

    // No resize borders when maximized
    if (SDL_GetWindowFlags(obj->_window) & SDL_WINDOW_MAXIMIZED) {
        return (y < obj->titlebarHeight()) ? SDL_HITTEST_DRAGGABLE : SDL_HITTEST_NORMAL;
    }

    enum RegionMask {
        client = 0b0000,
        left = 0b0001,
        right = 0b0010,
        top = 0b0100,
        bottom = 0b1000,
    };

    int result =
        ((x < borderX) ? left : 0) |
        ((x >= (width - borderX)) ? right : 0) |
        ((y < borderY) ? top : 0) |
        ((y >= (height - borderY)) ? bottom : 0);

    switch (result) {
        case left:              return SDL_HITTEST_RESIZE_LEFT;
        case right:             return SDL_HITTEST_RESIZE_RIGHT;
        case top:               return SDL_HITTEST_RESIZE_TOP;
        case bottom:            return SDL_HITTEST_RESIZE_BOTTOM;
        case top | left:        return SDL_HITTEST_RESIZE_TOPLEFT;
        case top | right:       return SDL_HITTEST_RESIZE_TOPRIGHT;
        case bottom | left:     return SDL_HITTEST_RESIZE_BOTTOMLEFT;
        case bottom | right:    return SDL_HITTEST_RESIZE_BOTTOMRIGHT;
        case client: {
            // Title bar area � allow dragging the window
            if (y < obj->titlebarHeight() && x < (width - obj->totalButtonWidth())) {
                return SDL_HITTEST_DRAGGABLE;
            }
            else {
                return SDL_HITTEST_NORMAL;
            }
        }
        default:                return SDL_HITTEST_NORMAL;
    }
}

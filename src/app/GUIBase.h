#pragma once

#include <app/FileDrag.h>
#include <vkutils/Device.h>
#include <vkutils/Instance.h>
#include <vkutils/PipelineCache.h>
#include <vkutils/Swapchain.h>

#include <SDL3/SDL.h>
#include <imgui.h>

#include <array>
#include <exception>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <vector>


class GUIBase
{
protected:
    enum Queue {
        Graphics = 0,
        Transfer,
        Count
    };

public:
    GUIBase(uint32_t width = 1280, uint32_t height = 720);
    ~GUIBase();

    virtual void run();
    virtual void quit() { _quit = true; }

    void minimizeWindow();
    void maximizeRestoreWindow();
    void requestOpenFile() { _requestOpenFile = true; }
    void requestSaveFile() { _requestSaveFile = true; }
    void requestSaveAsFile() { _requestSaveAsFile = true; }

    virtual void render(
        VkCommandBuffer commandBuffer,
        VkImage targetImage,
        VkImageView targetImageView,
        VkImageLayout targetLayout
    ) = 0;

    virtual void eventOpenFile(SDL_Window* parent) = 0;
    virtual void eventSaveFile(SDL_Window* parent) = 0;
    virtual void eventSaveAsFile(SDL_Window* parent) = 0;

    // Called on the main thread before each frame, while no command buffer is
    // being recorded.
    virtual void eventBeforeFrame() {}

    // While minimized, how long to sleep at most between two frames (ms):
    // -1 sleeps until the next event, a time keeps work going (a server).
    virtual int minimizedWakeMs() const { return -1; }

    // Every SDL event, before ImGui gets it (windows of the application
    // other than the main one: plugin editors).
    virtual void eventSdl(const SDL_Event& event) {}

protected:
    // Files dragged over the window from the file manager. The application
    // takes them once dropped, then calls endFileDrag().
    const FileDrag& fileDrag() const { return _fileDrag; }
    void endFileDrag() { _fileDrag = FileDrag(); }
    void setFileDrag(FileDrag drag) { _fileDrag = std::move(drag); }

    const GPU::Device& device() const { return *_pDevice; }
    VkInstance instance() const { return _pInstance->instance(); }
    SDL_Window* window() const { return _window; }

    // Video decoding queue, when the device has one (Vulkan Video). It is
    // the device queue of index videoDecodeQueue().
    bool hasVideoDecodeQueue() const { return _hasVideoDecodeQueue; }
    uint32_t videoDecodeQueue() const { return Queue::Count; }

    // Video encoding queue (Vulkan Video), only with a decoding queue: the
    // device queue of index videoEncodeQueue().
    bool hasVideoEncodeQueue() const { return _hasVideoEncodeQueue; }
    uint32_t videoEncodeQueue() const { return Queue::Count + 1; }

    // The other device queue of the graphics family, for background GPU
    // work (export). May be the graphics queue itself, see queueMutex().
    VkQueue backgroundQueue() const { return _pDevice->queue(Queue::Transfer); }

    // What the device was created with, for the libraries sharing it
    // (FFmpeg).
    const VkPhysicalDeviceFeatures2* deviceFeatures() const { return (const VkPhysicalDeviceFeatures2*)_deviceFeatures; }
    const std::vector<const char*>& instanceExtensions() const { return _instanceExtensions; }
    const std::vector<const char*>& deviceExtensions() const { return _deviceExtensions; }

    // Semaphores the next submission of the frame being rendered waits for
    // and signals, in addition to the swapchain ones. Values are ignored for
    // binary semaphores. Main thread only, during render().
    void addFrameWaitSemaphore(VkSemaphore semaphore, uint64_t value, VkPipelineStageFlags stage);
    void addFrameSignalSemaphore(VkSemaphore semaphore, uint64_t value);
    GPU::PipelineCache* pipelineCache() const { return _pPipelineCache; }
    std::string& gpuName() { return _gpuName; }
    bool borderlessWindow() const { return _borderlessWindow; }

    // Sets the horizontal range of the title bar, in pixels, which holds the
    // widgets of the application (the menus): as the title bar buttons, it
    // receives the clicks instead of dragging or resizing the window.
    void setTitlebarClientRange(float minX, float maxX)
    {
        _titlebarClientMinX = minX;
        _titlebarClientMaxX = maxX;
    }

    // Sets the ImGui window holding the title bar (the background window).
    // Any other window of the main viewport over the title bar receives the
    // clicks: a floating window moved there must stay movable.
    void setTitlebarWindow(ImGuiID id) { _titlebarWindowId = id; }

    // Mutex guarding the submissions to the given queue. Both roles share the
    // same mutex when the device gave them the same VkQueue.
    std::mutex& queueMutex(Queue queue);

    // Mutex guarding the submissions to any queue of the device.
    std::mutex& queueMutex(VkQueue queue);

private:
    void createSDL();
    void destroySDL();

    // Records, submits and presents one frame.
    void renderFrame();

    // Redraws the window during a live resize (see sdlEventWatch()).
    void onLiveResize();

    void createVulkan();
    void destroyVulkan();

    void createVulkanInstance();
    void destroyVulkanInstance();

    void createVulkanDevice();
    void destroyVulkanDevice();

    void createVulkanSwapchain();
    void destroyVulkanSwapchain();

    void createVulkanCommandBuffers();
    void destroyVulkanCommandBuffers();

    void createVulkanRenderPass();
    void destroyVulkanRenderPass();

    void createVulkanFramebuffers();
    void destroyVulkanFramebuffers();

    void createImGui();
    void resizeImGui();

    // Applies the current display scale to the ImGui style, from the
    // unscaled _baseStyle (never rescales an already scaled style, which
    // would accumulate rounding errors).
    void applyDisplayScale();
    void cmdRenderImGui(VkCommandBuffer commandBuffer, uint32_t imageIndex);
    void destroyImGui();

    void onWindowResized();

#ifndef NDEBUG
    VkDebugUtilsMessengerEXT _debugUtilsMessenger = VK_NULL_HANDLE;

    static VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(
        VkDebugUtilsMessageSeverityFlagBitsEXT severity,
        VkDebugUtilsMessageTypeFlagsEXT type,
        const VkDebugUtilsMessengerCallbackDataEXT* callbackData,
        void* userData);
#endif // _DEBUG

    static void* allocRequiredFeatures(bool videoMaintenance1);
    static void freeRequiredFeatures(void* features);

    // Leaves out of the features those the device does not have (some
    // drivers lack a few, used only when there), logged.
    static void keepSupportedFeatures(VkPhysicalDevice physicalDevice, void* features);
    static SDL_HitTestResult SDLCALL sdlHitTest(SDL_Window* win, const SDL_Point* area, void* data);

    // On Windows the main loop is blocked in the OS modal loop while the
    // window is resized or moved. SDL then sends live resize expose events
    // from a timer, which this watch uses to keep the content up to date.
    static bool SDLCALL sdlEventWatch(void* userdata, SDL_Event* event);

    // createSDL
    SDL_Window* _window = nullptr;
    float _displayScale = 1.f;

    // ImGui style at scale 1, see applyDisplayScale().
    ImGuiStyle _baseStyle;

    // createVulkan

    // createVulkanInstance
    GPU::Instance* _pInstance = nullptr;
    
    // createVulkanDevice
    VkSurfaceKHR _surface = VK_NULL_HANDLE;
    GPU::Device* _pDevice = nullptr;
    GPU::PipelineCache* _pPipelineCache = nullptr;

    // createVulkanSwapchain
    GPU::Swapchain* _pSwapchain = nullptr;

    // createVulkanCommandBuffers
protected: // TODO : make private
    std::array<VkCommandPool, Queue::Count> _commandPools;
    std::array<VkCommandBuffer, Queue::Count> _commandBuffers;
    std::array<VkFence, Queue::Count> _fences;

    // One mutex per distinct VkQueue, see queueMutex().
    std::map<VkQueue, std::unique_ptr<std::mutex>> _queueMutexes;

private:
    // createVulkanRenderPass
    VkRenderPass _renderPass = VK_NULL_HANDLE;
    
    // createVulkanFramebuffer
    std::vector<VkFramebuffer> _framebuffers;

    std::string _gpuName;

    // createVulkanInstance / createVulkanDevice, kept for queries.
    std::vector<const char*> _instanceExtensions;
    std::vector<const char*> _deviceExtensions;
    void* _deviceFeatures = nullptr;
    bool _hasVideoDecodeQueue = false;
    bool _hasVideoEncodeQueue = false;

    // See addFrameWaitSemaphore() and addFrameSignalSemaphore().
    std::vector<VkSemaphore> _frameWaitSemaphores;
    std::vector<uint64_t> _frameWaitValues;
    std::vector<VkPipelineStageFlags> _frameWaitStages;
    std::vector<VkSemaphore> _frameSignalSemaphores;
    std::vector<uint64_t> _frameSignalValues;

    bool _quit = false;
    FileDrag _fileDrag;
    bool _requestResize = true;

    // True while renderFrame() runs, so a live resize redraw never re-enters it.
    bool _inFrame = false;

    // Live resize redraws are throttled to keep resizing responsive.
    Uint64 _lastLiveResizeFrameNS = 0;
    static constexpr Uint64 LiveResizeFrameIntervalNS = SDL_NS_PER_SECOND / 30;

    // Exception thrown during a live resize redraw: it cannot cross SDL's C
    // callback, so it is rethrown from run().
    std::exception_ptr _liveResizeException;
    bool _isMaximized;
    bool _borderlessWindow = true;
    int _width, _height;
    bool _requestOpenFile = false;
    bool _requestSaveFile = false;
    bool _requestSaveAsFile = false;

    // See setTitlebarClientRange().
    float _titlebarClientMinX = 0.f;
    float _titlebarClientMaxX = 0.f;

    // See setTitlebarWindow().
    ImGuiID _titlebarWindowId = 0;

    // True when an ImGui window other than the title bar one covers the
    // given point of the main window (in window coordinates).
    bool windowOverTitlebar(float x, float y) const;

protected:
    // Title bar metrics, in pixels, for the current display scale.
    float titlebarHeight() const   { return 32.f * _displayScale; }
    float buttonWidth() const      { return 55.f * _displayScale; }
    float totalButtonWidth() const { return 3.f * buttonWidth(); }
};
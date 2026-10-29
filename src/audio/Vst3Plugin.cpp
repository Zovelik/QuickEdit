#include <audio/Vst3Plugin.h>

#include "public.sdk/source/common/memorystream.h"
#include "public.sdk/source/vst/hosting/hostclasses.h"
#include "public.sdk/source/vst/hosting/module.h"
#include "public.sdk/source/vst/hosting/parameterchanges.h"
#include "public.sdk/source/vst/hosting/processdata.h"
#include "pluginterfaces/base/funknownimpl.h"
#include "pluginterfaces/gui/iplugview.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivstmessage.h"
#include "pluginterfaces/vst/ivstprocesscontext.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>


using namespace Steinberg;
using namespace Steinberg::Vst;


namespace audio
{

namespace {

// The host context given to every plugin.
IPtr<HostApplication>& hostApplication()
{
    static IPtr<HostApplication> host = owned(new HostApplication());
    return host;
}


// Receives the parameter edits of the editor (main thread), for the
// processing (audio thread).
class ComponentHandler : public U::Implements<U::Directly<IComponentHandler>>
{
public:
    struct Change
    {
        ParamID id;
        ParamValue value;
    };

    tresult PLUGIN_API beginEdit(ParamID) override { return kResultOk; }
    tresult PLUGIN_API endEdit(ParamID) override { return kResultOk; }
    tresult PLUGIN_API restartComponent(int32) override { return kResultOk; }

    tresult PLUGIN_API performEdit(ParamID id, ParamValue value) override
    {
        std::scoped_lock lock(mutex);
        changes.push_back({ id, value });
        return kResultOk;
    }

    std::mutex mutex;
    std::vector<Change> changes;
};


// Resizes the editor window when the plugin asks.
class PlugFrame : public U::Implements<U::Directly<IPlugFrame>>
{
public:
    explicit PlugFrame(SDL_Window* window) : _window(window) {}

    tresult PLUGIN_API resizeView(IPlugView* view, ViewRect* newSize) override
    {
        if (!view || !newSize) {
            return kInvalidArgument;
        }

        SDL_SetWindowSize(_window, newSize->getWidth(), newSize->getHeight());
        view->onSize(newSize);
        return kResultOk;
    }

private:
    SDL_Window* _window;
};

} // namespace


struct Vst3Plugin::Impl
{
    VST3::Hosting::Module::Ptr module;
    IPtr<IComponent> component;
    IPtr<IAudioProcessor> processor;
    IPtr<IEditController> controller;
    bool singleComponent = false;

    IPtr<ComponentHandler> handler;
    HostProcessData processData;
    ParameterChanges inputChanges;
    ProcessContext context = {};
    bool prepared = false;

    IPtr<IPlugView> view;
    IPtr<PlugFrame> frame;

    ~Impl()
    {
        if (view) {
            view->setFrame(nullptr);
            view->removed();
            view = nullptr;
        }

        if (prepared) {
            processor->setProcessing(false);
            component->setActive(false);
            processData.unprepare();
        }

        // Disconnect the component and the controller.
        if (controller && !singleComponent) {
            FUnknownPtr<IConnectionPoint> componentPoint(component);
            FUnknownPtr<IConnectionPoint> controllerPoint(controller);

            if (componentPoint && controllerPoint) {
                componentPoint->disconnect(controllerPoint);
                controllerPoint->disconnect(componentPoint);
            }
        }

        if (controller) {
            controller->setComponentHandler(nullptr);

            if (!singleComponent) {
                controller->terminate();
            }
        }

        if (component) {
            component->terminate();
        }

        controller = nullptr;
        processor = nullptr;
        component = nullptr;

        // The module last: it holds the code of the objects above.
        module.reset();
    }
};


std::vector<PluginFile> findVst3Plugins()
{
    std::vector<std::filesystem::path> folders;

#ifdef _WIN32
    if (const char* common = std::getenv("CommonProgramFiles")) {
        folders.push_back(std::filesystem::path(common) / "VST3");
    }

    if (const char* local = std::getenv("LOCALAPPDATA")) {
        folders.push_back(std::filesystem::path(local) / "Programs" / "Common" / "VST3");
    }
#else
    if (const char* home = std::getenv("HOME")) {
        folders.push_back(std::filesystem::path(home) / ".vst3");
    }

    folders.push_back("/usr/lib/vst3");
    folders.push_back("/usr/local/lib/vst3");
#endif

    std::vector<PluginFile> plugins;

    for (const std::filesystem::path& folder : folders) {
        std::error_code error;

        for (auto it = std::filesystem::recursive_directory_iterator(folder, std::filesystem::directory_options::skip_permission_denied, error);
             !error && it != std::filesystem::recursive_directory_iterator(); it.increment(error)) {
            if (it->path().extension() != ".vst3") {
                continue;
            }

            // A bundle is a folder: not inside it.
            if (it->is_directory(error)) {
                it.disable_recursion_pending();
            }

            const std::u8string path = it->path().u8string();
            const std::u8string name = it->path().stem().u8string();
            plugins.push_back({ std::string(path.begin(), path.end()), std::string(name.begin(), name.end()) });
        }
    }

    std::sort(plugins.begin(), plugins.end(), [](const PluginFile& a, const PluginFile& b) { return a.name < b.name; });

    return plugins;
}


std::unique_ptr<Vst3Plugin> Vst3Plugin::load(const std::string& path, const std::string& classId, std::string& error)
{
    std::unique_ptr<Vst3Plugin> plugin(new Vst3Plugin());
    plugin->_impl = std::make_unique<Impl>();
    Impl& impl = *plugin->_impl;

    impl.module = VST3::Hosting::Module::create(path, error);

    if (!impl.module) {
        return nullptr;
    }

    const VST3::Hosting::PluginFactory& factory = impl.module->getFactory();
    const VST3::Hosting::ClassInfo* chosen = nullptr;
    const VST3::Hosting::PluginFactory::ClassInfos classes = factory.classInfos();

    for (const VST3::Hosting::ClassInfo& info : classes) {
        if (info.category() == kVstAudioEffectClass && (classId.empty() || info.ID().toString() == classId)) {
            chosen = &info;
            break;
        }
    }

    if (!chosen) {
        error = "No audio effect in this plugin";
        return nullptr;
    }

    plugin->_name = chosen->name();
    plugin->_classId = chosen->ID().toString();

    impl.component = factory.createInstance<IComponent>(chosen->ID());

    if (!impl.component || impl.component->initialize(hostApplication()) != kResultOk) {
        error = "Could not create the plugin";
        impl.component = nullptr;
        return nullptr;
    }

    impl.processor = FUnknownPtr<IAudioProcessor>(impl.component);

    if (!impl.processor) {
        error = "The plugin does not process audio";
        return nullptr;
    }

    // Controller: a separate object, or the component itself.
    TUID controllerId;

    if (impl.component->getControllerClassId(controllerId) == kResultTrue) {
        impl.controller = factory.createInstance<IEditController>(VST3::UID::fromTUID(controllerId));

        if (impl.controller && impl.controller->initialize(hostApplication()) != kResultOk) {
            impl.controller = nullptr;
        }
    }

    if (!impl.controller) {
        impl.controller = FUnknownPtr<IEditController>(impl.component);
        impl.singleComponent = impl.controller != nullptr;
    }

    if (impl.controller) {
        if (!impl.singleComponent) {
            FUnknownPtr<IConnectionPoint> componentPoint(impl.component);
            FUnknownPtr<IConnectionPoint> controllerPoint(impl.controller);

            if (componentPoint && controllerPoint) {
                componentPoint->connect(controllerPoint);
                controllerPoint->connect(componentPoint);
            }
        }

        impl.handler = owned(new ComponentHandler());
        impl.controller->setComponentHandler(impl.handler);

        // The controller starts from the state of the component.
        MemoryStream stream;

        if (impl.component->getState(&stream) == kResultOk) {
            stream.seek(0, IBStream::kIBSeekSet, nullptr);
            impl.controller->setComponentState(&stream);
        }
    }

    // Stereo in, stereo out, when the plugin agrees.
    SpeakerArrangement stereo = SpeakerArr::kStereo;
    impl.processor->setBusArrangements(&stereo, 1, &stereo, 1);

    if (impl.component->getBusCount(kAudio, kInput) > 0) {
        impl.component->activateBus(kAudio, kInput, 0, true);
    }

    if (impl.component->getBusCount(kAudio, kOutput) > 0) {
        impl.component->activateBus(kAudio, kOutput, 0, true);
    }

    return plugin;
}


Vst3Plugin::~Vst3Plugin()
{
    closeEditor();
    _impl.reset();
}


bool Vst3Plugin::prepare(double sampleRate, int maxFrames)
{
    Impl& impl = *_impl;

    if (impl.prepared) {
        impl.processor->setProcessing(false);
        impl.component->setActive(false);
        impl.processData.unprepare();
        impl.prepared = false;
    }

    ProcessSetup setup = { kRealtime, kSample32, maxFrames, sampleRate };

    if (impl.processor->setupProcessing(setup) != kResultOk) {
        return false;
    }

    if (impl.component->setActive(true) != kResultOk) {
        return false;
    }

    impl.processData.prepare(*impl.component, maxFrames, kSample32);
    impl.inputChanges.setMaxParameters(64);
    impl.processData.inputParameterChanges = &impl.inputChanges;

    impl.context = {};
    impl.context.sampleRate = sampleRate;
    impl.context.tempo = 120.0;
    impl.context.timeSigNumerator = 4;
    impl.context.timeSigDenominator = 4;
    impl.context.state = ProcessContext::kPlaying | ProcessContext::kTempoValid | ProcessContext::kTimeSigValid;
    impl.processData.processContext = &impl.context;

    impl.processor->setProcessing(true);
    impl.prepared = true;

    return true;
}


void Vst3Plugin::process(float* samples, int frames, int64_t position)
{
    Impl& impl = *_impl;

    if (!impl.prepared || frames <= 0) {
        return;
    }

    HostProcessData& data = impl.processData;
    data.numSamples = frames;
    impl.context.projectTimeSamples = position;

    // Parameter edits of the editor; skipped for this block if the editor
    // holds the lock.
    impl.inputChanges.clearQueue();

    if (impl.handler && impl.handler->mutex.try_lock()) {
        for (const ComponentHandler::Change& change : impl.handler->changes) {
            int32 queueIndex = 0;

            if (IParamValueQueue* queue = impl.inputChanges.addParameterData(change.id, queueIndex)) {
                int32 pointIndex = 0;
                queue->addPoint(0, change.value, pointIndex);
            }
        }

        impl.handler->changes.clear();
        impl.handler->mutex.unlock();
    }

    // Deinterleave into the input bus.
    if (data.numInputs > 0) {
        AudioBusBuffers& input = data.inputs[0];

        for (int32 c = 0; c < input.numChannels; c++) {
            float* channel = input.channelBuffers32[c];

            for (int i = 0; i < frames; i++) {
                channel[i] = (input.numChannels == 1) ? 0.5f * (samples[2 * i] + samples[2 * i + 1]) : samples[2 * i + std::min(c, 1)];
            }
        }
    }

    if (impl.processor->process(data) != kResultOk || data.numOutputs == 0) {
        return;
    }

    // Interleave the output bus back.
    const AudioBusBuffers& output = data.outputs[0];

    if (output.numChannels <= 0) {
        return;
    }

    const float* left = output.channelBuffers32[0];
    const float* right = output.channelBuffers32[std::min<int32>(1, output.numChannels - 1)];

    for (int i = 0; i < frames; i++) {
        samples[2 * i] = left[i];
        samples[2 * i + 1] = right[i];
    }
}


std::vector<uint8_t> Vst3Plugin::saveState()
{
    Impl& impl = *_impl;

    // Size and bytes of the component state, then of the controller state.
    MemoryStream componentState;
    MemoryStream controllerState;

    impl.component->getState(&componentState);

    if (impl.controller && !impl.singleComponent) {
        impl.controller->getState(&controllerState);
    }

    std::vector<uint8_t> state;

    for (MemoryStream* stream : { &componentState, &controllerState }) {
        const uint32_t size = (uint32_t)stream->getSize();

        for (int b = 0; b < 4; b++) {
            state.push_back((uint8_t)(size >> (8 * b)));
        }

        state.insert(state.end(), (const uint8_t*)stream->getData(), (const uint8_t*)stream->getData() + size);
    }

    return state;
}


bool Vst3Plugin::loadState(const std::vector<uint8_t>& state)
{
    Impl& impl = *_impl;
    size_t offset = 0;

    auto next = [&](std::vector<uint8_t>& out) {
        if (offset + 4 > state.size()) {
            return false;
        }

        uint32_t size = 0;

        for (int b = 0; b < 4; b++) {
            size |= (uint32_t)state[offset + b] << (8 * b);
        }

        offset += 4;

        if (offset + size > state.size()) {
            return false;
        }

        out.assign(state.begin() + (ptrdiff_t)offset, state.begin() + (ptrdiff_t)(offset + size));
        offset += size;
        return true;
    };

    std::vector<uint8_t> componentState;
    std::vector<uint8_t> controllerState;

    if (!next(componentState) || !next(controllerState)) {
        return false;
    }

    if (!componentState.empty()) {
        MemoryStream stream(componentState.data(), (TSize)componentState.size());
        impl.component->setState(&stream);

        if (impl.controller) {
            MemoryStream copy(componentState.data(), (TSize)componentState.size());
            impl.controller->setComponentState(&copy);
        }
    }

    if (!controllerState.empty() && impl.controller && !impl.singleComponent) {
        MemoryStream stream(controllerState.data(), (TSize)controllerState.size());
        impl.controller->setState(&stream);
    }

    return true;
}


bool Vst3Plugin::hasEditor() const
{
    return _impl->controller != nullptr;
}


bool Vst3Plugin::openEditor()
{
#ifdef _WIN32
    Impl& impl = *_impl;

    if (_window) {
        SDL_RaiseWindow(_window);
        return true;
    }

    if (!impl.controller) {
        return false;
    }

    impl.view = owned(impl.controller->createView(ViewType::kEditor));

    if (!impl.view || impl.view->isPlatformTypeSupported(kPlatformTypeHWND) != kResultTrue) {
        impl.view = nullptr;
        return false;
    }

    ViewRect rect;

    if (impl.view->getSize(&rect) != kResultOk || rect.getWidth() <= 0 || rect.getHeight() <= 0) {
        rect = ViewRect(0, 0, 640, 400);
    }

    const SDL_WindowFlags flags = (impl.view->canResize() == kResultTrue) ? SDL_WINDOW_RESIZABLE : 0;
    _window = SDL_CreateWindow(_name.c_str(), rect.getWidth(), rect.getHeight(), flags);

    if (!_window) {
        impl.view = nullptr;
        return false;
    }

    void* hwnd = SDL_GetPointerProperty(SDL_GetWindowProperties(_window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);

    impl.frame = owned(new PlugFrame(_window));
    impl.view->setFrame(impl.frame);

    if (impl.view->attached(hwnd, kPlatformTypeHWND) != kResultOk) {
        impl.view->setFrame(nullptr);
        impl.view = nullptr;
        SDL_DestroyWindow(_window);
        _window = nullptr;
        return false;
    }

    return true;
#else
    // Linux editors need an X11 run loop: not yet.
    return false;
#endif
}


void Vst3Plugin::closeEditor()
{
    if (!_window) {
        return;
    }

    Impl& impl = *_impl;

    if (impl.view) {
        impl.view->removed();
        impl.view->setFrame(nullptr);
        impl.view = nullptr;
    }

    impl.frame = nullptr;

    SDL_DestroyWindow(_window);
    _window = nullptr;
}


bool Vst3Plugin::handleEvent(const SDL_Event& event)
{
    if (!_window || event.window.windowID != SDL_GetWindowID(_window)) {
        return false;
    }

    if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
        closeEditor();
    }
    else if (event.type == SDL_EVENT_WINDOW_RESIZED && _impl->view) {
        ViewRect rect(0, 0, event.window.data1, event.window.data2);
        _impl->view->onSize(&rect);
    }

    return true;
}

}

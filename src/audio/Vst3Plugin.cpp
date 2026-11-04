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

#include <core/Guard.h>
#include <core/Json.h>
#include <core/Paths.h>

#include <algorithm>
#include <type_traits>

#ifdef _WIN32
#include <excpt.h>
#endif
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>


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


namespace {

// Runs code of a plugin. On Windows an access violation or an exception
// in it is caught (structured exception handling) and false returned,
// instead of ending QuickEdit. sehCall() holds no object with a destructor,
// as __try requires.
#ifdef _WIN32
bool sehCall(void (*thunk)(void*), void* context) noexcept
{
    __try {
        thunk(context);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
#endif


template<typename Function>
bool pluginCall(Function&& function)
{
#ifdef _WIN32
    return sehCall([](void* context) { (*static_cast<std::remove_reference_t<Function>*>(context))(); }, &function);
#else
    function();
    return true;
#endif
}

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


namespace {

std::vector<PluginClass> listPluginClassesUnguarded(const std::string& path, std::string& error)
{
    std::vector<PluginClass> classes;
    VST3::Hosting::Module::Ptr module = VST3::Hosting::Module::create(path, error);

    if (!module) {
        return classes;
    }

    for (const VST3::Hosting::ClassInfo& info : module->getFactory().classInfos()) {
        // The effects, not the instruments.
        const std::string subCategories = info.subCategoriesString();

        if (info.category() != kVstAudioEffectClass || subCategories.find("Instrument") != std::string::npos) {
            continue;
        }

        classes.push_back({ path, info.ID().toString(), info.name(), info.vendor(), subCategories });
    }

    if (classes.empty()) {
        error = "No audio effect in this plugin";
    }

    return classes;
}

} // namespace


std::vector<PluginClass> listPluginClasses(const std::string& path, std::string& error)
{
    std::vector<PluginClass> classes;

    // A plugin that crashes when it is opened is left out, not QuickEdit.
    if (!pluginCall([&]() { classes = listPluginClassesUnguarded(path, error); })) {
        error = "the plugin crashed when opened";
        classes.clear();
    }

    return classes;
}


namespace {

// Date and size of a plugin file, or of the files of a bundle: a change
// means it was updated.
std::string fileStamp(const std::filesystem::path& path)
{
    std::error_code error;
    int64_t newest = 0;
    uintmax_t size = 0;

    auto add = [&](const std::filesystem::path& file) {
        const auto time = std::filesystem::last_write_time(file, error);

        if (!error) {
            newest = std::max<int64_t>(newest, (int64_t)time.time_since_epoch().count());
        }

        const uintmax_t fileSize = std::filesystem::file_size(file, error);
        size += error ? 0 : fileSize;
    };

    if (std::filesystem::is_directory(path, error)) {
        for (auto it = std::filesystem::recursive_directory_iterator(path, error);
             !error && it != std::filesystem::recursive_directory_iterator(); it.increment(error)) {
            if (it->is_regular_file(error)) {
                add(it->path());
            }
        }
    }
    else {
        add(path);
    }

    return std::to_string(newest) + ":" + std::to_string(size);
}


std::filesystem::path pluginCachePath()
{
    return core::configDirectory() / "vst3-plugins.json";
}

} // namespace


PluginScanner::~PluginScanner()
{
    _stop = true;

    if (_thread.joinable()) {
        _thread.join();
    }
}


void PluginScanner::start(bool rescan)
{
    if (_scanning.load()) {
        return;
    }

    if (_thread.joinable()) {
        _thread.join();
    }

    _scanning = true;
    // An error out of the scan ends it with the plugins found so far.
    _thread = std::thread([this, rescan]() {
        if (!core::guarded("Plugin scan", [&]() { run(rescan); })) {
            _scanning = false;
        }
    });
}


std::vector<PluginClass> PluginScanner::classes() const
{
    std::scoped_lock lock(_mutex);
    return _classes;
}


std::string PluginScanner::current() const
{
    std::scoped_lock lock(_mutex);
    return _current;
}


void PluginScanner::run(bool rescan)
{
    // The cache: per file, its stamp and its effects.
    struct Entry
    {
        std::string stamp;
        std::vector<PluginClass> classes;
    };

    std::map<std::string, Entry> cache;

    if (!rescan) {
        try {
            std::ifstream file(pluginCachePath(), std::ios::binary);
            std::stringstream text;
            text << file.rdbuf();

            const core::Json json = core::Json::parse(text.str());
            const core::Json& files = json["files"];

            for (size_t i = 0; i < files.size(); i++) {
                Entry& entry = cache[files[i]["path"].toString()];
                entry.stamp = files[i]["stamp"].toString();

                const core::Json& classes = files[i]["classes"];

                for (size_t j = 0; j < classes.size(); j++) {
                    entry.classes.push_back({ files[i]["path"].toString(), classes[j]["id"].toString(), classes[j]["name"].toString(),
                        classes[j]["vendor"].toString(), classes[j]["subCategories"].toString() });
                }
            }
        }
        catch (const std::exception&) {
            cache.clear();
        }
    }

    std::vector<PluginClass> found;
    core::Json files = core::Json::array();

    for (const PluginFile& file : findVst3Plugins()) {
        if (_stop.load()) {
            break;
        }

        const std::filesystem::path path = std::filesystem::u8path(file.path);
        const std::string stamp = fileStamp(path);
        auto cached = cache.find(file.path);

        if (cached == cache.end() || cached->second.stamp != stamp) {
            {
                std::scoped_lock lock(_mutex);
                _current = file.name;
            }

            Entry entry;
            entry.stamp = stamp;

            // A plugin may throw while loading: it is left out.
            try {
                std::string error;
                entry.classes = listPluginClasses(file.path, error);

                if (entry.classes.empty()) {
                    SDL_Log("Plugin %s: %s", file.path.c_str(), error.c_str());
                }
            }
            catch (...) {
                SDL_Log("Plugin %s: could not be loaded", file.path.c_str());
            }

            cached = cache.insert_or_assign(file.path, std::move(entry)).first;
        }

        core::Json json = core::Json::object();
        json["path"] = file.path;
        json["stamp"] = stamp;
        json["classes"] = core::Json::array();

        for (const PluginClass& info : cached->second.classes) {
            core::Json item = core::Json::object();
            item["id"] = info.classId;
            item["name"] = info.name;
            item["vendor"] = info.vendor;
            item["subCategories"] = info.subCategories;
            json["classes"].push_back(item);
            found.push_back(info);
        }

        files.push_back(json);
    }

    std::sort(found.begin(), found.end(), [](const PluginClass& a, const PluginClass& b) { return a.name < b.name; });

    if (!_stop.load()) {
        core::Json json = core::Json::object();
        json["files"] = files;

        std::ofstream file(pluginCachePath(), std::ios::binary);
        file << json.dump();
    }

    {
        std::scoped_lock lock(_mutex);
        _classes = std::move(found);
        _current.clear();
    }

    SDL_Log("Plugins: %d effects", (int)_classes.size());
    _scanning = false;
}


std::unique_ptr<Vst3Plugin> Vst3Plugin::load(const std::string& path, const std::string& classId, std::string& error)
{
    std::unique_ptr<Vst3Plugin> plugin;

    // A plugin that crashes when it is opened is not used.
    if (!pluginCall([&]() { plugin = loadUnguarded(path, classId, error); })) {
        error = "the plugin crashed when opened";
        (void)plugin.release();
    }

    return plugin;
}


std::unique_ptr<Vst3Plugin> Vst3Plugin::loadUnguarded(const std::string& path, const std::string& classId, std::string& error)
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
    // A crashed plugin is not called again, not even to be released.
    if (_crashed) {
        (void)_impl.release();
        return;
    }

    closeEditor();
    _impl.reset();
}


void Vst3Plugin::crash(const char* during)
{
    if (!_crashed.exchange(true)) {
        core::reportError("VST3 plugin " + _name, std::string("it crashed while ") + during +
            " and is turned off (the sound goes through it unchanged). Save the project and restart QuickEdit to use it again.");
    }
}


bool Vst3Plugin::prepare(double sampleRate, int maxFrames)
{
    bool result = false;

    if (!_crashed && !pluginCall([&]() { result = prepareUnguarded(sampleRate, maxFrames); })) {
        crash("starting");
    }

    return result && !_crashed;
}


void Vst3Plugin::process(float* samples, int frames, int64_t position)
{
    if (!_crashed && !pluginCall([&]() { processUnguarded(samples, frames, position); })) {
        crash("processing the sound");
    }
}


std::vector<uint8_t> Vst3Plugin::saveState()
{
    std::vector<uint8_t> state;

    if (!_crashed && !pluginCall([&]() { state = saveStateUnguarded(); })) {
        crash("saving its settings");
    }

    return state;
}


bool Vst3Plugin::loadState(const std::vector<uint8_t>& state)
{
    bool result = false;

    if (!_crashed && !pluginCall([&]() { result = loadStateUnguarded(state); })) {
        crash("loading its settings");
    }

    return result && !_crashed;
}


bool Vst3Plugin::openEditor()
{
    bool result = false;

    if (!_crashed && !pluginCall([&]() { result = openEditorUnguarded(); })) {
        crash("opening its editor");
    }

    return result && !_crashed;
}


void Vst3Plugin::closeEditor()
{
    if (!_crashed && !pluginCall([&]() { closeEditorUnguarded(); })) {
        crash("closing its editor");
    }
}


bool Vst3Plugin::prepareUnguarded(double sampleRate, int maxFrames)
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


void Vst3Plugin::processUnguarded(float* samples, int frames, int64_t position)
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


std::vector<uint8_t> Vst3Plugin::saveStateUnguarded()
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


bool Vst3Plugin::loadStateUnguarded(const std::vector<uint8_t>& state)
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

        out.assign(state.begin() + (std::ptrdiff_t)offset, state.begin() + (std::ptrdiff_t)(offset + size));
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


bool Vst3Plugin::openEditorUnguarded()
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


void Vst3Plugin::closeEditorUnguarded()
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

#include <app/GUI.h>
#include <app/I18n.h>
#include <core/Paths.h>
#include <app/AppIcon.h>
#include <config.h>

#include <fonts/icons.h>
#include <fonts/IconsMaterialDesign.h>

#include <TextEditor.h>

#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_stdlib.h>

#include <audio/Vst3Plugin.h>
#include <core/Layers.h>
#include <core/Subtitles.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <ctime>
#include <fstream>
#include <sstream>


using namespace core;


namespace {

// Settings of the application (language...), in the settings folder.
std::filesystem::path settingsPath()
{
    return configDirectory() / "settings.json";
}


Json loadSettings()
{
    std::ifstream file(settingsPath(), std::ios::binary);
    std::stringstream text;
    text << file.rdbuf();

    try {
        return text.str().empty() ? Json::object() : Json::parse(text.str());
    }
    catch (const std::exception&) {
        return Json::object();
    }
}


void saveSetting(const std::string& key, const Json& value)
{
    Json settings = loadSettings();

    if (!settings.isObject()) {
        settings = Json::object();
    }

    settings[key] = value;
    std::ofstream(settingsPath(), std::ios::binary) << settings.dump();
}


// Flat, square toolbar button with a tooltip (also shown when disabled).
// Stays on the same line for the next toolbar item.
bool toolbarButton(
    const char* icon,
    const char* tooltip,
    const char* shortcut,
    float size,
    bool enabled = true,
    bool selected = false)
{
    ImGui::BeginDisabled(!enabled);

    if (selected) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
    }

    const bool pressed = ImGui::Button(icon, ImVec2(size, size));
    tooltip = tr(tooltip);

    if (selected) {
        ImGui::PopStyleColor();
    }

    ImGui::EndDisabled();

    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled)) {
        // Tooltips use the regular font size, not the toolbar's.
        ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase);

        if (shortcut) {
            ImGui::SetTooltip("%s (%s)", tooltip, shortcut);
        }
        else {
            ImGui::SetTooltip("%s", tooltip);
        }

        ImGui::PopFont();
    }

    ImGui::SameLine();

    return pressed;
}


void toolbarSeparator()
{
    ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
    ImGui::SameLine();
}


// Flat button of the toolbars of the panels, as tall as the other widgets
// of the line (combos, sliders); square when it is an icon alone. The
// selected one (a tool, a toggle) is highlighted. Stays on the same line.
bool panelButton(const char* label, const char* tooltip, bool selected = false, bool enabled = true)
{
    // An icon is one character of the private use area: 3 bytes in UTF-8.
    const char* end = ImGui::FindRenderedTextEnd(label);
    const bool iconOnly = (end - label) == 3 && (unsigned char)label[0] == 0xEE;
    const ImVec2 size(iconOnly ? ImGui::GetFrameHeight() : 0.f, ImGui::GetFrameHeight());

    ImGui::PushStyleColor(ImGuiCol_Button, selected ? ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive) : ImVec4(0.f, 0.f, 0.f, 0.f));
    ImGui::BeginDisabled(!enabled);
    const bool pressed = ImGui::Button(tr(label), size);
    ImGui::EndDisabled();
    ImGui::PopStyleColor();

    if (tooltip && ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("%s", tr(tooltip));
    }

    ImGui::SameLine();
    return pressed;
}


// Space between two groups of a panel toolbar, with a thin line.
void panelSeparator()
{
    const ImGuiStyle& style = ImGui::GetStyle();
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const float x = std::floor(pos.x + style.ItemSpacing.x) + 0.5f;
    const float inset = 0.2f * ImGui::GetFrameHeight();

    ImGui::GetWindowDrawList()->AddLine(ImVec2(x, pos.y + inset), ImVec2(x, pos.y + ImGui::GetFrameHeight() - inset),
        ImGui::GetColorU32(ImGuiCol_Separator));
    ImGui::Dummy(ImVec2(2.f * style.ItemSpacing.x, ImGui::GetFrameHeight()));
    ImGui::SameLine();
}


// Begins a menu of the menu bar whose highlight fills the bar from top to
// bottom, so it joins the menu opened below without any gap.
bool beginBarMenu(const char* label)
{
    // The highlight is as tall as the label plus the vertical item spacing,
    // while the bar is as tall as the label plus twice the frame padding.
    const float spacingY = 2.0f * ImGui::GetStyle().FramePadding.y;

    ImGui::PushStyleVarY(ImGuiStyleVar_ItemSpacing, spacingY);
    const bool open = ImGui::BeginMenu(label);
    ImGui::PopStyleVar();

    return open;
}


std::string pathToUtf8(const std::filesystem::path& path)
{
    const std::u8string u8 = path.u8string();
    return std::string(u8.begin(), u8.end());
}


const char* mediaIcon(MediaType type)
{
    switch (type) {
        case MediaType::Audio: return ICON_MD_AUDIOTRACK;
        case MediaType::Image: return ICON_MD_IMAGE;
        default:               return ICON_MD_MOVIE;
    }
}



} // namespace



GUI::GUI(const std::filesystem::path& projectPath, double startSeconds, Id selectedClip)
    : GUIBase(1600, 1000)
{
    ImGuiIO& io = ImGui::GetIO();
    ImGuiStyle& style = ImGui::GetStyle();

    // Material icons merged into the main font (Inter, loaded by GUIBase).
    const float fontSize = style.FontScaleDpi * 16.f;
    const float iconFontSize = fontSize;

    ImFontConfig cfgIcons;
    cfgIcons.MergeMode = true;
    cfgIcons.PixelSnapH = true;
    cfgIcons.GlyphMinAdvanceX = fontSize;
    cfgIcons.GlyphOffset.y = iconFontSize / 5.f;

    io.Fonts->AddFontFromMemoryCompressedTTF(
        Icons_compressed_data,
        Icons_compressed_size,
        iconFontSize,
        &cfgIcons
    );

    // A monospace font of the system for the code, if one is found.
    {
        std::vector<std::filesystem::path> candidates;

#ifdef _WIN32
        if (const char* windows = std::getenv("WINDIR")) {
            candidates.push_back(std::filesystem::path(windows) / "Fonts" / "CascadiaMono.ttf");
            candidates.push_back(std::filesystem::path(windows) / "Fonts" / "consola.ttf");
        }
#else
        candidates = {
            "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
            "/usr/share/fonts/TTF/DejaVuSansMono.ttf",
            "/usr/share/fonts/dejavu/DejaVuSansMono.ttf",
            "/usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf",
        };
#endif

        for (const std::filesystem::path& path : candidates) {
            std::error_code ec;

            if (std::filesystem::exists(path, ec)) {
                _codeFont = io.Fonts->AddFontFromFileTTF(path.string().c_str(), 0.f);
                break;
            }
        }
    }

    // The language chosen before, else the system's.
    {
        const std::string language = loadSettings()["language"].toString();

        if (language.empty() || !i18n::setLanguage(language)) {
            i18n::setLanguage(i18n::systemLanguage());
        }
    }

    _timeline.waveformOf = [this](Id mediaId, int stream) { return _media.waveform(mediaId, stream); };
    _timeline.thumbnailsOf = [this](Id mediaId) { return thumbnailsOf(mediaId); };
    _timeline.userShaders = &_userShaders;
    _inspector.userShaders = &_userShaders;
    _inspector.shaderError = [this](const std::string& id) { return _compositor ? _compositor->shaderError(id) : std::string(); };
    _inspector.openShaderFolder = [this]() { openShaderFolder(); };
    _inspector.editShader = [this](const std::string& id) { editShader(id); };
    _inspector.newShader = [this](ShaderKind kind) { return newShader(kind); };
    applyShaderReload();

    createVideo();

    _history.reset(_project);

    if (!projectPath.empty()) {
        openProject(projectPath);
    }

    seek(secondsToTime(startSeconds));

    if (_project.findClip(selectedClip)) {
        _selection = { selectedClip };
    }

    updateWindowTitle();
}


GUI::~GUI()
{
    // Before the GPU objects it uses.
    _exporter.reset();

    // The GPU may still use the video frames and the renderer resources.
    vkDeviceWaitIdle(device().device());
}


void GUI::createVideo()
{
    // Decoding on the GPU when the device has a video decoding queue.
    if (hasVideoDecodeQueue()) {
        media::VulkanDeviceDesc desc;
        desc.instance = instance();
        desc.physicalDevice = device().physicalDevice();
        desc.device = device().device();
        desc.features = deviceFeatures();
        desc.instanceExtensions = instanceExtensions();
        desc.deviceExtensions = deviceExtensions();

        // Queues created per family: FFmpeg uses the indices 0 to count - 1.
        auto queueCount = [this](uint32_t family) {
            std::vector<VkQueue> queues;

            const uint32_t last = hasVideoEncodeQueue() ? videoEncodeQueue() : videoDecodeQueue();

            for (uint32_t i = 0; i <= last; i++) {
                if (device().queueFamilyIdx(i) == family &&
                    std::find(queues.begin(), queues.end(), device().queue(i)) == queues.end()) {
                    queues.push_back(device().queue(i));
                }
            }

            return (uint32_t)queues.size();
        };

        const uint32_t graphicsFamily = device().queueFamilyIdx(Queue::Graphics);
        const uint32_t decodeFamily = device().queueFamilyIdx(videoDecodeQueue());

        desc.queueFamilies.push_back({ graphicsFamily, queueCount(graphicsFamily), VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT });
        desc.queueFamilies.push_back({ decodeFamily, queueCount(decodeFamily), VK_QUEUE_VIDEO_DECODE_BIT_KHR });

        if (hasVideoEncodeQueue()) {
            const uint32_t encodeFamily = device().queueFamilyIdx(videoEncodeQueue());
            desc.queueFamilies.push_back({ encodeFamily, queueCount(encodeFamily), VK_QUEUE_VIDEO_ENCODE_BIT_KHR });
        }

        desc.lockQueue = [this](uint32_t family, uint32_t index, bool lock) {
            VkQueue queue = VK_NULL_HANDLE;
            vkGetDeviceQueue(device().device(), family, index, &queue);

            std::mutex& mutex = queueMutex(queue);

            if (lock) {
                mutex.lock();
            }
            else {
                mutex.unlock();
            }
        };

        try {
            _hwDevice = std::make_unique<media::HwDevice>(desc);
        }
        catch (const std::exception& e) {
            SDL_Log("Video decoding on the CPU: %s", e.what());
        }
    }

    // The default font of the texts is the one of the interface (Inter),
    // decompressed by ImGui when it loaded it.
    const ImFontConfig& interface = ImGui::GetIO().Fonts->Sources[0];
    const uint8_t* fontData = (const uint8_t*)interface.FontData;
    _defaultFontData.assign(fontData, fontData + interface.FontDataSize);
    _fonts = std::make_unique<FontLibrary>(_defaultFontData);

    _compositor = std::make_unique<Compositor>(device(), *pipelineCache(), *_fonts);
}


void GUI::cmdCompose(VkCommandBuffer commandBuffer)
{
    // The previous frame is done with its resources (GUIBase waited for it).
    _compositor->beginFrame();

    const Sequence& sequence = _project.sequence();

    for (auto& [key, decoder] : _decoders) {
        decoder.renderer->beginFrame();
        decoder.used = false;
    }

    _viewerLoading = false;

    // The layers of a level (the sequence, or a nested one).
    std::function<std::vector<Compositor::Layer>(const std::vector<LayerRequest>&, const Sequence&)> build;

    build = [&](const std::vector<LayerRequest>& requests, const Sequence& level) {
        std::vector<Compositor::Layer> layers;

        for (const LayerRequest& request : requests) {
            const Clip& clip = *request.clip;

            Compositor::Layer layer = { &clip, request.localTime, nullptr, 0, 0,
                request.opacity, request.offsetX, request.offsetY, request.scale, request.wipe };
            layer.trackEffects = &level.videoTracks[request.track].videoEffects;
                layer.trackBlend = level.videoTracks[request.track].blend;
            layer.transitionRole = request.transitionRole;
            layer.transitionProgress = request.transitionProgress;
            layer.transitionShader = request.transition ? _project.findShader(request.transition->shader) : nullptr;

            if (clip.kind == ClipKind::Text) {
                layer.animateLetters = _textEditor.clip() != clip.id;
                layers.push_back(layer);
                continue;
            }

            // Generators: the shader makes the picture.
            if (clip.kind == ClipKind::Generator) {
                layer.generator = _project.findShader(clip.shader);

                if (layer.generator) {
                    layers.push_back(layer);
                }

                continue;
            }

            // A nested sequence: its own layers, composited first.
            if (clip.kind == ClipKind::Sequence) {
                if (request.nested) {
                    layer.children = build(request.children, *request.nested);
                    layer.childWidth = request.nested->width;
                    layer.childHeight = request.nested->height;
                    layers.push_back(std::move(layer));
                }

                continue;
            }

            const MediaItem* media = _project.findMedia(clip.mediaId);

            if (!media || media->type == MediaType::Audio) {
                continue;
            }

            if (!media->probed) {
                _viewerLoading |= _media.state(media->id).status != media::MediaStatus::Error;
                continue;
            }

            Decoder& decoder = _decoders[request.key];

            if (!decoder.player) {
                decoder.player = std::make_unique<media::VideoPlayer>(_hwDevice ? _hwDevice->context() : nullptr);
                decoder.renderer = std::make_unique<VideoFrameRenderer>(device(), *pipelineCache());
                decoder.renderer->beginFrame();
            }

            decoder.used = true;
            decoder.idleFrames = 0;

            media::VideoPlayer& player = *decoder.player;
            VideoFrameRenderer& renderer = *decoder.renderer;

            player.request(media->id, media->path, request.sourceTime, _playing && !request.still);

            const std::shared_ptr<media::VideoFrame> frame = player.frame();
            VideoFrameRenderer::Sync sync;

            // Not decoded yet (opening the file, seeking), unless it cannot be.
            if (!frame || !frame->covers(request.sourceTime)) {
                _viewerLoading |= !player.failed(media->id);
            }

            if (!frame || !renderer.cmdRender(commandBuffer, frame, sync) || !renderer.image()) {
                continue;
            }

            if (_loggedVideoMedia.insert(media->id).second) {
                SDL_Log("Decoding %s on the %s", media->name().c_str(), frame->hwFrame ? "GPU (Vulkan Video)" : "CPU");
            }

            for (const VideoFrameRenderer::SemaphoreOp& wait : sync.waits) {
                addFrameWaitSemaphore(wait.semaphore, wait.value, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
            }

            for (const VideoFrameRenderer::SemaphoreOp& signal : sync.signals) {
                addFrameSignalSemaphore(signal.semaphore, signal.value);
            }

            layer.image = renderer.image();
            layer.width = renderer.width();
            layer.height = renderer.height();
            layers.push_back(layer);
        }

        return layers;
    };

    std::vector<Compositor::Layer> layers = build(
        layersAt(sequence, _playhead, [this](Id id) { return _project.findSequence(id); }), sequence);

    // The subtitles, over everything (kept until the frame is recorded).
    _subtitleClips = subtitleClipsAt(sequence, _playhead);

    for (const Clip& clip : _subtitleClips) {
        Compositor::Layer layer = { &clip, _playhead - clip.start, nullptr, 0, 0, 1.f, 0.f, 0.f, 1.f, 1.f };
        layers.push_back(layer);
    }

    // The decoders with nothing to show stop; those unused for a while go
    // (the GPU is done with their images: GUIBase waited for the frame).
    for (auto it = _decoders.begin(); it != _decoders.end();) {
        if (!it->second.used) {
            it->second.player->clear();

            if (++it->second.idleFrames > 600) {
                it = _decoders.erase(it);
                continue;
            }
        }

        ++it;
    }

    _compositor->cmdComposite(commandBuffer, sequence.width, sequence.height, layers);
}


void GUI::render(
    VkCommandBuffer commandBuffer,
    VkImage targetImage,
    VkImageView targetImageView,
    VkImageLayout targetLayout
)
{
    updateMedia();
    updatePlayback();
    cmdCompose(commandBuffer);

    beginMainWindow();
    menuBar();
    toolbar();
    shortcuts();

    // Host a dockspace inside the main window so the panels can be docked
    // around each other.
    const ImGuiID dockSpaceId = ImGui::GetID("MainDockSpace");

    if (!_dockLayoutInitialized) {
        _dockLayoutInitialized = true;
        dockLayout(dockSpaceId);
    }

    ImGui::DockSpace(dockSpaceId);

    fileDragPreview();

    mediaPanel();
    viewerPanel();
    inspectorPanel();
    timelinePanel();
    keyframesPanel();

    if (_testRecordSeconds > 0.0 && _recorder.isOpen()) {
        if (!_recorder.recording()) {
            startRecording();
        }
        else if (_recorder.recordedFrames() >= (int64_t)(_testRecordSeconds * audio::Recorder::SampleRate)) {
            stopRecording();
            _testRecordSeconds = 0.0;
            _testQuitTicks = SDL_GetTicks() + 3000;
        }
    }

    // After a test recording: quits without saving, so that the test never
    // has to kill the application.
    if (_testQuitTicks && SDL_GetTicks() > _testQuitTicks) {
        GUIBase::quit();
    }

    if (_testScreenSeconds > 0.0 && _captureSourcesListed && _testScreenSource >= 0 && _testScreenSource < (int)_captureSources.size()) {
        _captureSource = _captureSources[(size_t)_testScreenSource];
        _testScreenSource = -1;
    }

    if (_testScreenSeconds > 0.0 && SDL_GetTicks() > _testScreenStartTicks) {
        if (!_screenRecording) {
            startScreenRecording();

            if (!_screenRecording) {
                _testScreenSeconds = 0.0;
                _testQuitTicks = SDL_GetTicks() + 3000;
            }
        }
        else if (_screenRecorder.seconds() >= _testScreenSeconds) {
            stopScreenRecording();
            _testScreenSeconds = 0.0;
            _testQuitTicks = SDL_GetTicks() + 3000;
        }
    }

    recorderWindow();
    screenRecorderWindow();
    subtitleGeneratorWindow();
    shaderEditorWindow();

    fileDropped();

    exportPopup();
    projectPropertiesPopup();
    errorPopup();
    unsavedChangesPopup();

    endMainWindow();

    updateWindowTitle();

    // ------------------------------------------------------------------------
    // Barriers to final layout
    // ------------------------------------------------------------------------

    const VkImageMemoryBarrier barrierImageToPresent = {
         VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, nullptr,
         VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,          // srcAccessMask
         VK_ACCESS_MEMORY_READ_BIT,                     // dstAccesMask
         VK_IMAGE_LAYOUT_UNDEFINED,                     // oldLayout
         targetLayout,                                  // newLayout
         VK_QUEUE_FAMILY_IGNORED,                       // srcQueueFamilyIndex
         VK_QUEUE_FAMILY_IGNORED,                       // dstQueueFamilyIndex
         targetImage,                                   // image
         {                                              // imageSubresourceRange
             VK_IMAGE_ASPECT_COLOR_BIT,                 // aspect
             0,                                         // mipLevel
             1,                                         // levelCount
             0,                                         // baseArrayLayer
             1,                                         // layerCount
         }
    };

    vkCmdPipelineBarrier(
        commandBuffer,                                  // commandBuffer
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,  // srcStageMask
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,             // dstStageMask
        0,                                              // dependencyFlags
        0, nullptr,                                     // pMemoryBarriers
        0, nullptr,                                     // pBufferMemoryBarriers
        1, &barrierImageToPresent                       // pImageMemoryBarriers
    );
}


void GUI::dockLayout(ImGuiID dockSpaceId)
{
    // On first launch (no saved ImGui settings), or when the saved dockspace
    // was never split: media and viewer side by side above the timeline, the
    // inspector on the right. Must happen before DockSpace() creates the node.
    const ImGuiDockNode* node = ImGui::DockBuilderGetNode(dockSpaceId);

    if (node != nullptr && node->IsSplitNode()) {
        return;
    }

    ImGui::DockBuilderRemoveNode(dockSpaceId);
    ImGui::DockBuilderAddNode(dockSpaceId, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockSpaceId, ImGui::GetContentRegionAvail());

    ImGuiID topNode = dockSpaceId;
    const ImGuiID timelineNode = ImGui::DockBuilderSplitNode(topNode, ImGuiDir_Down, 0.5f, nullptr, &topNode);

    ImGuiID viewerNode = topNode;
    const ImGuiID mediaNode = ImGui::DockBuilderSplitNode(viewerNode, ImGuiDir_Left, 0.25f, nullptr, &viewerNode);
    const ImGuiID inspectorNode = ImGui::DockBuilderSplitNode(viewerNode, ImGuiDir_Right, 0.3f, nullptr, &viewerNode);

    ImGui::DockBuilderDockWindow("###Media", mediaNode);
    ImGui::DockBuilderDockWindow("###Viewer", viewerNode);
    ImGui::DockBuilderDockWindow("###Inspector", inspectorNode);
    ImGui::DockBuilderDockWindow("###Timeline", timelineNode);
    ImGui::DockBuilderDockWindow("###Keyframes", timelineNode);
    ImGui::DockBuilderFinish(dockSpaceId);
}


void GUI::beginMainWindow()
{
    const ImGuiViewport* pViewport = ImGui::GetMainViewport();

    ImGui::SetNextWindowSize(pViewport->Size);
    ImGui::SetNextWindowPos(pViewport->Pos);

    // The Main window fills the whole viewport, so keep it square.
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));

    // In a borderless window, the menu bar is the title bar: ImGui sizes it
    // from the frame padding when the window begins.
    if (borderlessWindow()) {
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, titlebarFramePadding());
    }

    // The Main window is the background of the application: it never comes
    // in front of the floating (undocked) windows, which it would hide for
    // good since it fills the viewport.
    ImGui::Begin("Main", nullptr,
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_MenuBar |
        ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoBringToFrontOnFocus);

    setTitlebarWindow(ImGui::GetCurrentWindow()->ID);

    if (borderlessWindow()) {
        ImGui::PopStyleVar();
    }

    ImGui::PopStyleVar(2);
}


void GUI::endMainWindow()
{
    ImGui::End();
}


ImVec2 GUI::titlebarFramePadding() const
{
    const ImGuiStyle& style = ImGui::GetStyle();

    // The menu bar is as tall as the font plus twice the vertical padding.
    // Whole pixels keep the text sharp. Both terms follow the display scale.
    const float paddingY = std::floor(0.5f * (titlebarHeight() - ImGui::GetFontSize()));

    return ImVec2(style.FramePadding.x, std::max(paddingY, style.FramePadding.y));
}


void GUI::menuBar()
{
    const bool inTitlebar = borderlessWindow();
    const bool hasSelection = !_selection.empty();

    // The menus rely on the frame padding to center their label in the bar
    // and to open right below it: keep the one the window began with. It has
    // no effect on the menu items themselves.
    if (inTitlebar) {
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, titlebarFramePadding());
    }

    if (ImGui::BeginMenuBar()) {
        if (inTitlebar) {
            titlebarIcon();
        }

        const float menusMinX = ImGui::GetCursorScreenPos().x;
        bool menuOpen = false;

        if (beginBarMenu(tr("File"))) {
            menuOpen = true;

            if (ImGui::MenuItem(tr(ICON_MD_NOTE_ADD    " New Project"),        "Ctrl+N"))       { _requestedAction = PendingAction::NewProject; }
            if (ImGui::MenuItem(tr(ICON_MD_FOLDER_OPEN " Open Project..."),    "Ctrl+O"))       { requestOpenFile(); }
            if (ImGui::MenuItem(tr(ICON_MD_SAVE        " Save Project"),       "Ctrl+S"))       { requestSaveFile(); }
            if (ImGui::MenuItem(tr(ICON_MD_SAVE_AS     " Save Project As..."), "Ctrl+Shift+S")) { requestSaveAsFile(); }
            ImGui::Separator();
            if (ImGui::MenuItem(tr(ICON_MD_UPLOAD_FILE " Import Media..."),    "Ctrl+I"))       { requestImportMedia(); }
            if (ImGui::MenuItem(tr(ICON_MD_MOVIE_CREATION " Export Video..."), "Ctrl+E"))       { _requestExportPopup = true; }
            ImGui::Separator();

            if (ImGui::MenuItem(tr(ICON_MD_SUBTITLES " Import Subtitles (.srt)..."))) {
                _subtitleDialogSaves = false;
                SDL_ShowOpenFileDialog(GUI::onSubtitleFile, this, window(), subtitleFilters, SDL_arraysize(subtitleFilters), nullptr, false);
            }

            if (ImGui::MenuItem(tr(ICON_MD_SUBTITLES " Export Subtitles (.srt)..."), nullptr, false, !subtitleCues(_project.sequence()).empty())) {
                _subtitleDialogSaves = true;
                SDL_ShowSaveFileDialog(GUI::onSubtitleFile, this, window(), subtitleFilters, SDL_arraysize(subtitleFilters), nullptr);
            }
            ImGui::Separator();
            if (ImGui::MenuItem(tr(ICON_MD_EXIT_TO_APP " Exit"),               "Alt+F4"))       { quit(); }
            ImGui::EndMenu();
        }

        if (beginBarMenu(tr("Edit"))) {
            menuOpen = true;

            const std::string undoLabel = std::string(tr(ICON_MD_UNDO " Undo ")) + tr(_history.undoLabel());
            const std::string redoLabel = std::string(tr(ICON_MD_REDO " Redo ")) + tr(_history.redoLabel());

            if (ImGui::MenuItem(undoLabel.c_str(), "Ctrl+Z", nullptr, _history.canUndo())) { undo(); }
            if (ImGui::MenuItem(redoLabel.c_str(), "Ctrl+Y", nullptr, _history.canRedo())) { redo(); }
            ImGui::Separator();
            if (ImGui::MenuItem(tr(ICON_MD_CONTENT_CUT " Split at Playhead"), "Ctrl+K"))                    { splitAtPlayhead(); }
            if (ImGui::MenuItem(tr(ICON_MD_PAUSE_CIRCLE " Freeze Frame"),   "Shift+F"))                   { freezeFrame(); }
            if (ImGui::MenuItem(tr(ICON_MD_COMPARE_ARROWS " Add / Remove Transition"), "Ctrl+D"))          { toggleTransition(); }
            if (ImGui::MenuItem(tr(ICON_MD_DELETE       " Delete"),           "Del",       nullptr, hasSelection)) { deleteSelection(false); }
            if (ImGui::MenuItem(tr(ICON_MD_DELETE_SWEEP " Ripple Delete"),    "Shift+Del", nullptr, hasSelection)) { deleteSelection(true); }
            ImGui::Separator();

            if (ImGui::MenuItem(tr(ICON_MD_TEXT_FIELDS " Add Text"), "T")) { addText(); }
            if (ImGui::MenuItem(tr(ICON_MD_SUBTITLES " Add Subtitle"), "Shift+T")) { addSubtitle(); }
            if (ImGui::MenuItem(tr(ICON_MD_RECORD_VOICE_OVER " Generate Subtitles..."))) { _showSubtitleGenerator = true; }

            if (ImGui::BeginMenu(tr(ICON_MD_AUTO_AWESOME " Add Generator"))) {
                for (const ShaderDef& shader : builtinShaders()) {
                    if (shader.kind == ShaderKind::Generator && ImGui::MenuItem(shader.name.c_str())) addGenerator(shader);
                }

                bool separated = false;

                for (const ShaderDef& shader : _userShaders) {
                    if (shader.kind != ShaderKind::Generator) continue;
                    if (!std::exchange(separated, true)) ImGui::Separator();
                    if (ImGui::MenuItem(shader.name.c_str())) addGenerator(shader);
                }

                ImGui::Separator();

                if (ImGui::MenuItem(tr(ICON_MD_ADD " New Generator Shader..."))) {
                    if (const std::optional<ShaderDef> shader = newShader(ShaderKind::Generator)) addGenerator(*shader);
                }

                if (ImGui::MenuItem(tr(ICON_MD_ADD " New Transition Shader..."))) { newShader(ShaderKind::Transition); }

                if (ImGui::BeginMenu(tr(ICON_MD_EDIT " Edit Shader"), !_userShaders.empty())) {
                    for (const ShaderDef& shader : _userShaders) {
                        if (ImGui::MenuItem(shader.name.c_str(), shader.kind == ShaderKind::Generator ? "Generator" : "Transition")) {
                            editShader(shader.id);
                        }
                    }

                    ImGui::EndMenu();
                }

                if (ImGui::MenuItem(tr(ICON_MD_FOLDER_OPEN " Open Shaders Folder"))) { openShaderFolder(); }
                if (ImGui::MenuItem(tr(ICON_MD_REFRESH " Reload Shaders"))) { reloadShaders(true); }
                ImGui::EndMenu();
            }
            ImGui::MenuItem(tr(ICON_MD_MIC " Record Voice-Over..."), "R", &_showRecorder);
            ImGui::MenuItem(tr(ICON_MD_SCREEN_SHARE " Record Screen..."), "Shift+R", &_showScreenRecorder);
            ImGui::Separator();
            if (ImGui::MenuItem(tr(ICON_MD_LIBRARY_ADD " Add Video Track"))) { addTrack(TrackType::Video); }
            if (ImGui::MenuItem(tr(ICON_MD_PLAYLIST_ADD " Add Audio Track"))) { addTrack(TrackType::Audio); }
            if (ImGui::MenuItem(tr(ICON_MD_SUBTITLES " Add Subtitle Track"))) { addTrack(TrackType::Subtitle); }
            ImGui::Separator();
            if (ImGui::MenuItem(tr(ICON_MD_VIDEO_SETTINGS " Project Properties..."))) { _requestProjectProperties = true; }
            ImGui::Separator();

            bool snapping = _timeline.snapping();
            if (ImGui::MenuItem(tr(ICON_MD_VERTICAL_ALIGN_CENTER " Snapping"), "N", &snapping)) { _timeline.setSnapping(snapping); }

            if (ImGui::BeginMenu(tr(ICON_MD_HANDYMAN " Timeline Tool"))) {
                for (const TimelineToolInfo& info : timelineTools()) {
                    if (ImGui::MenuItem(tr(info.label), info.shortcut, _timeline.tool() == info.tool)) { _timeline.setTool(info.tool); }
                }

                ImGui::EndMenu();
            }

            ImGui::EndMenu();
        }

        if (beginBarMenu(tr("Playback"))) {
            menuOpen = true;

            if (ImGui::MenuItem(_playing ? tr(ICON_MD_PAUSE " Pause") : tr(ICON_MD_PLAY_ARROW " Play"), tr("Space"))) { togglePlayback(); }
            ImGui::Separator();
            if (ImGui::MenuItem(tr(ICON_MD_SKIP_PREVIOUS   " Go to Start"),    "Home"))  { seek(0); }
            if (ImGui::MenuItem(tr(ICON_MD_SKIP_NEXT       " Go to End"),      "End"))   { seek(_project.sequence().duration()); }
            if (ImGui::MenuItem(tr(ICON_MD_NAVIGATE_BEFORE " Previous Frame"), "Left"))  { stepFrames(-1); }
            if (ImGui::MenuItem(tr(ICON_MD_NAVIGATE_NEXT   " Next Frame"),     "Right")) { stepFrames(1); }
            ImGui::EndMenu();
        }

        if (beginBarMenu(tr("View"))) {
            menuOpen = true;

            if (ImGui::MenuItem(tr(ICON_MD_ZOOM_IN    " Zoom In Timeline"),  "Ctrl++")) { _timeline.setZoom(_timeline.zoom() * 1.5f); }
            if (ImGui::MenuItem(tr(ICON_MD_ZOOM_OUT   " Zoom Out Timeline"), "Ctrl+-")) { _timeline.setZoom(_timeline.zoom() / 1.5f); }
            if (ImGui::MenuItem(tr(ICON_MD_FIT_SCREEN " Fit Sequence"),      "Ctrl+0")) { _timeline.requestZoomToFit(); }
            ImGui::Separator();
            if (ImGui::MenuItem(tr(ICON_MD_TIMELINE " Keyframes"))) { ImGui::SetWindowFocus("###Keyframes"); }
            ImGui::Separator();

            if (ImGui::BeginMenu(tr(ICON_MD_LANGUAGE " Language"))) {
                for (const i18n::Language& language : i18n::languages()) {
                    if (ImGui::MenuItem(language.name.c_str(), nullptr, language.code == i18n::language()) &&
                        i18n::setLanguage(language.code)) {
                        saveSetting("language", language.code);
                    }
                }

                ImGui::EndMenu();
            }

            ImGui::Separator();
            if (ImGui::MenuItem(tr("Reset Layout"))) { _dockLayoutInitialized = false; ImGui::DockBuilderRemoveNode(ImGui::GetID("MainDockSpace")); }
            ImGui::EndMenu();
        }

        if (inTitlebar) {
            const ImGuiViewport* pViewport = ImGui::GetMainViewport();
            const float menusMaxX = ImGui::GetCursorScreenPos().x;

            // The rest of the title bar drags the window, which the OS
            // handles without ImGui seeing any click. While a menu is open,
            // the whole title bar is left to ImGui instead so that a click
            // there closes the menu.
            if (menuOpen) {
                setTitlebarClientRange(0.f, pViewport->Size.x);
            }
            else {
                setTitlebarClientRange(menusMinX - pViewport->Pos.x, menusMaxX - pViewport->Pos.x);
            }

            // Project name, centered in the title bar.
            const ImRect bar = ImGui::GetCurrentWindow()->MenuBarRect();
            const ImVec2 titleSize = ImGui::CalcTextSize(_windowTitle.c_str());
            const float titleX = std::max(menusMaxX + ImGui::GetStyle().ItemSpacing.x, bar.GetCenter().x - 0.5f * titleSize.x);

            if (titleX + titleSize.x < bar.Max.x - totalButtonWidth()) {
                ImGui::GetWindowDrawList()->AddText(
                    ImVec2(titleX, bar.GetCenter().y - 0.5f * titleSize.y),
                    ImGui::GetColorU32(ImGuiCol_TextDisabled),
                    _windowTitle.c_str());
            }

            titlebarButtons();
        }

        ImGui::EndMenuBar();
    }

    if (inTitlebar) {
        ImGui::PopStyleVar();
    }
}


void GUI::titlebarIcon()
{
    const ImGuiStyle& style = ImGui::GetStyle();
    const ImRect bar = ImGui::GetCurrentWindow()->MenuBarRect();

    // 20 pixels in the 32 pixels title bar at 100% scale, centered
    // vertically.
    const float size = std::round(0.625f * bar.GetHeight());
    const float marginY = std::floor(0.5f * (bar.GetHeight() - size));

    // The theme item spacing separates the icon from the window border and
    // from the menus.
    const float spacingX = style.ItemSpacing.x;
    const ImVec2 pos(bar.Min.x + spacingX, bar.Min.y + marginY);

    drawAppIcon(ImGui::GetWindowDrawList(), pos, size);

    // The menus start after the icon.
    ImGui::SetCursorScreenPos(ImVec2(pos.x + size + spacingX, bar.Min.y));
}


void GUI::titlebarButtons()
{
    const ImRect bar = ImGui::GetCurrentWindow()->MenuBarRect();
    const float barButtonWidth = buttonWidth();
    const ImVec2 buttonSize(barButtonWidth, bar.GetHeight());

    // The buttons fill the bar, square, up to the edge of the window.
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.0f);

    // Minimize & Resize buttons
    ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(255, 255, 255, 20));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, IM_COL32(255, 255, 255, 50));
    {
        ImGui::SetCursorScreenPos(ImVec2(bar.Max.x - 3 * barButtonWidth, bar.Min.y));
        if (ImGui::Button(ICON_MD_MINIMIZE, buttonSize)) { minimizeWindow(); }

        ImGui::SetCursorScreenPos(ImVec2(bar.Max.x - 2 * barButtonWidth, bar.Min.y));
        if (ImGui::Button(ICON_MD_CHECK_BOX_OUTLINE_BLANK, buttonSize)) { maximizeRestoreWindow(); }
    }
    ImGui::PopStyleColor(3);

    // Close button
    ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(255, 0, 0, 125));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(255, 50, 50, 200));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, IM_COL32(255, 0, 0, 255));
    {
        ImGui::SetCursorScreenPos(ImVec2(bar.Max.x - barButtonWidth, bar.Min.y));
        if (ImGui::Button(ICON_MD_CLOSE, buttonSize)) { quit(); }
    }
    ImGui::PopStyleColor(3);

    ImGui::PopStyleVar();

    // Title separator
    ImGui::GetWindowDrawList()->AddLine(
        ImVec2(bar.Min.x, bar.Max.y - 1.f),
        ImVec2(bar.Max.x, bar.Max.y - 1.f),
        ImGui::GetColorU32(ImGuiCol_Separator), 1.0f
    );
}


void GUI::toolbar()
{
    const ImGuiStyle& style = ImGui::GetStyle();
    const bool hasSelection = !_selection.empty();

    // Larger font for the icons.
    ImGui::PushFont(nullptr, style.FontSizeBase * 1.5f);

    const float buttonSize = ImGui::GetFontSize() + 2.0f * style.FramePadding.y;
    const ImVec2 padding(style.ItemSpacing.x * 0.5f, style.FramePadding.y);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, padding);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(style.ItemInnerSpacing.x * 0.5f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(0, 0, 0, 0));

    ImGui::BeginChild(
        "Toolbar",
        ImVec2(0.0f, buttonSize + 2.0f * padding.y),
        ImGuiChildFlags_AlwaysUseWindowPadding,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse
    );

    if (toolbarButton(ICON_MD_NOTE_ADD,    "New Project",     "Ctrl+N", buttonSize)) { _requestedAction = PendingAction::NewProject; }
    if (toolbarButton(ICON_MD_FOLDER_OPEN, "Open Project...", "Ctrl+O", buttonSize)) { requestOpenFile(); }
    if (toolbarButton(ICON_MD_SAVE,        "Save Project",    "Ctrl+S", buttonSize)) { requestSaveFile(); }

    toolbarSeparator();

    if (toolbarButton(ICON_MD_UPLOAD_FILE, "Import Media...", "Ctrl+I", buttonSize)) { requestImportMedia(); }
    if (toolbarButton(ICON_MD_MOVIE_CREATION, "Export Video...", "Ctrl+E", buttonSize)) { _requestExportPopup = true; }
    if (toolbarButton(ICON_MD_MIC, "Record Voice-Over...", "R", buttonSize, true, _showRecorder)) { _showRecorder = !_showRecorder; }
    if (toolbarButton(ICON_MD_SCREEN_SHARE, "Record Screen...", "Shift+R", buttonSize, true, _showScreenRecorder)) { _showScreenRecorder = !_showScreenRecorder; }

    toolbarSeparator();

    if (toolbarButton(ICON_MD_UNDO, "Undo", "Ctrl+Z", buttonSize, _history.canUndo())) { undo(); }
    if (toolbarButton(ICON_MD_REDO, "Redo", "Ctrl+Y", buttonSize, _history.canRedo())) { redo(); }

    toolbarSeparator();

    if (toolbarButton(ICON_MD_TEXT_FIELDS, "Add Text", "T", buttonSize))                             { addText(); }
    if (toolbarButton(ICON_MD_PAUSE_CIRCLE, "Freeze Frame", "Shift+F", buttonSize))                  { freezeFrame(); }
    if (toolbarButton(ICON_MD_COMPARE_ARROWS, "Add / Remove Transition", "Ctrl+D", buttonSize))      { toggleTransition(); }

    toolbarSeparator();

    if (toolbarButton(ICON_MD_CONTENT_CUT, "Split at Playhead", "Ctrl+K", buttonSize))               { splitAtPlayhead(); }
    if (toolbarButton(ICON_MD_DELETE,       "Delete",            "Del",       buttonSize, hasSelection)) { deleteSelection(false); }
    if (toolbarButton(ICON_MD_DELETE_SWEEP, "Ripple Delete",     "Shift+Del", buttonSize, hasSelection)) { deleteSelection(true); }
    if (toolbarButton(ICON_MD_VERTICAL_ALIGN_CENTER, "Snapping", "N", buttonSize, true, _timeline.snapping())) {
        _timeline.setSnapping(!_timeline.snapping());
    }

    ImGui::PopFont();

    ImGui::EndChild();

    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
}


void GUI::shortcuts()
{
    // The text editor of the viewer has the keyboard.
    if (_textEditor.active()) {
        return;
    }

    // As well as a dialog or a menu.
    if (ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel)) {
        return;
    }

    const ImGuiInputFlags global = ImGuiInputFlags_RouteGlobal;
    const ImGuiInputFlags repeat = ImGuiInputFlags_RouteGlobal | ImGuiInputFlags_Repeat;

    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_N, global)) { _requestedAction = PendingAction::NewProject; }
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_I, global)) { requestImportMedia(); }
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_E, global)) { _requestExportPopup = true; }
    if (ImGui::Shortcut(ImGuiKey_R, global))                 { _showRecorder = !_showRecorder; }
    if (ImGui::Shortcut(ImGuiMod_Shift | ImGuiKey_T, global)) { addSubtitle(); }
    if (ImGui::Shortcut(ImGuiMod_Shift | ImGuiKey_R, global)) { _showScreenRecorder = !_showScreenRecorder; }

    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Z, repeat)) { undo(); }
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Y, repeat) ||
        ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Z, repeat)) {
        redo();
    }

    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_K, global)) { splitAtPlayhead(); }
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_D, global)) { toggleTransition(); }

    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Equal,                  repeat) ||
        ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Equal, repeat) ||
        ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_KeypadAdd,              repeat)) {
        _timeline.setZoom(_timeline.zoom() * 1.5f);
    }

    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Minus,          repeat) ||
        ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_KeypadSubtract, repeat)) {
        _timeline.setZoom(_timeline.zoom() / 1.5f);
    }

    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_0,       global) ||
        ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Keypad0, global)) {
        _timeline.requestZoomToFit();
    }

    // Single key shortcuts must not fire while typing in a text field.
    if (ImGui::GetIO().WantTextInput) {
        return;
    }

    if (ImGui::Shortcut(ImGuiKey_Space, global))       { togglePlayback(); }
    if (ImGui::Shortcut(ImGuiKey_Home, global))        { seek(0); }
    if (ImGui::Shortcut(ImGuiKey_End, global))         { seek(_project.sequence().duration()); }
    if (ImGui::Shortcut(ImGuiKey_LeftArrow, repeat))   { stepFrames(-1); }
    if (ImGui::Shortcut(ImGuiKey_RightArrow, repeat))  { stepFrames(1); }
    // Delete: the gap selected in the timeline, or the selected clips. The
    // media panel handles its own.
    if (!_mediaPanelFocused) {
        if (ImGui::Shortcut(ImGuiKey_Delete, global) || ImGui::Shortcut(ImGuiKey_Backspace, global)) {
            if (_timeline.hasGap()) {
                closeGap();
            }
            else {
                deleteSelection(false);
            }
        }

        if (ImGui::Shortcut(ImGuiMod_Shift | ImGuiKey_Delete, global) ||
            ImGui::Shortcut(ImGuiMod_Shift | ImGuiKey_Backspace, global)) {
            deleteSelection(true);
        }
    }
    if (ImGui::Shortcut(ImGuiKey_N, global))           { _timeline.setSnapping(!_timeline.snapping()); }
    if (ImGui::Shortcut(ImGuiKey_V, global))           { _timeline.setTool(TimelineView::Tool::Select); }
    if (ImGui::Shortcut(ImGuiKey_C, global))           { _timeline.setTool(TimelineView::Tool::Blade); }
    if (ImGui::Shortcut(ImGuiKey_Y, global))           { _timeline.setTool(TimelineView::Tool::Slip); }
    if (ImGui::Shortcut(ImGuiKey_A, global))           { _timeline.setTool(TimelineView::Tool::SelectForward); }
    if (ImGui::Shortcut(ImGuiKey_T, global))           { addText(); }

    if (ImGui::Shortcut(ImGuiKey_Enter, global) && _selection.size() >= 1) {
        const Clip* clip = _project.findClip(_selection.front());

        if (clip && clip->kind == ClipKind::Text) {
            beginTextEditing(clip->id);
        }
    }
    if (ImGui::Shortcut(ImGuiMod_Shift | ImGuiKey_F, global)) { freezeFrame(); }
}


// ----------------------------------------------------------------------------
// Panels
// ----------------------------------------------------------------------------

void GUI::mediaPanel()
{
    if (!ImGui::Begin(tr("Media###Media"))) {
        ImGui::End();
        return;
    }

    if (ImGui::Button(tr(ICON_MD_UPLOAD_FILE " Import..."))) {
        requestImportMedia();
    }

    ImGui::SameLine();

    // A folder in the selected one.
    std::string newFolderIn;
    std::string removeFolder;
    std::vector<std::pair<Id, std::string>> moves;
    bool newFolder = false;

    if (ImGui::Button(ICON_MD_CREATE_NEW_FOLDER)) {
        newFolder = true;
    }

    ImGui::SetItemTooltip(tr("New folder (in the selected one)"));
    ImGui::SameLine();
    ImGui::TextDisabled(tr("%zu item(s)"), _project.media().size());

    const ImGuiTableFlags flags =
        ImGuiTableFlags_RowBg |
        ImGuiTableFlags_BordersInnerV |
        ImGuiTableFlags_Resizable |
        ImGuiTableFlags_ScrollY;

    std::vector<Id> removeMedia;

    // Where each media was drawn, for the rectangle selection.
    struct ItemRow
    {
        Id id;
        float top;
        float bottom;
    };

    std::vector<ItemRow> itemRows;

    // Dropped or moved: the selection with the media, when it is part of it.
    auto moveSelection = [&](Id mediaId, const std::string& folder) {
        if (mediaSelected(mediaId)) {
            for (Id id : _mediaSelection) moves.emplace_back(id, folder);
        }
        else {
            moves.emplace_back(mediaId, folder);
        }
    };

    if (ImGui::BeginTable("##media", 2, flags)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn(tr("Name"), ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn(tr("Duration"), ImGuiTableColumnFlags_WidthFixed, ImGui::CalcTextSize("00:00:00:00").x);
        ImGui::TableHeadersRow();

        // A media: its row.
        auto drawItem = [&](const MediaItem& item) {
            ImGui::PushID((int)item.id);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();

            const std::string label = std::string(mediaIcon(item.type)) + " " + item.name();
            const bool missing = !std::filesystem::exists(item.path);
            const media::MediaState state = _media.state(item.id);

            if (missing) {
                ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(230, 90, 80, 255));
            }

            // The picture of a video or an image, before its name.
            const TimelineView::Thumbnails thumbnails = thumbnailsOf(item.id);
            const float pictureHeight = 2.f * ImGui::GetTextLineHeight();

            if (thumbnails.ready > 0) {
                const int index = thumbnails.image ? 0 : std::min(thumbnails.ready, thumbnails.count) / 3;
                const float u = (float)(index % thumbnails.columns) * thumbnails.tileU;
                const float v = (float)(index / thumbnails.columns) * thumbnails.tileV;

                ImGui::Image(thumbnails.texture, ImVec2(pictureHeight * 16.f / 9.f, pictureHeight),
                    ImVec2(u, v), ImVec2(u + thumbnails.tileU, v + thumbnails.tileV));
                ImGui::SameLine();
            }

            if (ImGui::Selectable(label.c_str(), mediaSelected(item.id), ImGuiSelectableFlags_SpanAllColumns,
                    ImVec2(0.f, thumbnails.ready > 0 ? pictureHeight : 0.f))) {
                // Ctrl: added to the selection, or taken out of it.
                if (ImGui::GetIO().KeyCtrl) {
                    if (mediaSelected(item.id)) {
                        std::erase(_mediaSelection, item.id);
                    }
                    else {
                        _mediaSelection.push_back(item.id);
                    }
                }
                else {
                    _mediaSelection = { item.id };
                }

                _selectedMedia = item.id;
                _selectedFolder = item.folder;
            }

            // Where it is, for the rectangle selection.
            itemRows.push_back({ item.id, ImGui::GetItemRectMin().y, ImGui::GetItemRectMax().y });

            if (missing) {
                ImGui::PopStyleColor();
            }

            // Placed on the timeline once its real duration is known.
            if (item.probed && ImGui::BeginDragDropSource()) {
                ImGui::SetDragDropPayload(TimelineView::MediaPayload, &item.id, sizeof(item.id));
                ImGui::TextUnformatted(label.c_str());
                ImGui::EndDragDropSource();
            }

            if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) {
                ImGui::BeginTooltip();
                ImGui::TextUnformatted(pathToUtf8(item.path).c_str());

                if (missing) {
                    ImGui::TextColored(ImVec4(0.9f, 0.35f, 0.3f, 1.f), tr(ICON_MD_WARNING " File not found"));
                }
                else if (state.status == media::MediaStatus::Error) {
                    ImGui::TextColored(ImVec4(0.9f, 0.35f, 0.3f, 1.f), ICON_MD_WARNING " %s", state.error.c_str());
                }
                else if (!item.probed) {
                    ImGui::TextDisabled(tr("Analyzing..."));
                }
                else {
                    if (item.type != MediaType::Audio) {
                        ImGui::Text(tr("%u x %u, %.3f fps"), item.width, item.height, item.frameRate.toDouble());
                    }

                    if (item.hasAudio) {
                        ImGui::Text(tr("%u Hz, %u channel(s)"), item.sampleRate, item.channels);
                    }

                    if (state.probe) {
                        const bool both = !state.probe->videoCodec.empty() && !state.probe->audioCodec.empty();
                        ImGui::TextDisabled("%s%s%s", state.probe->videoCodec.c_str(), both ? " / " : "", state.probe->audioCodec.c_str());
                    }
                }

                ImGui::EndTooltip();
            }

            // Another media dropped on it: into its folder.
            if (ImGui::BeginDragDropTarget()) {
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(TimelineView::MediaPayload)) {
                    moveSelection(*(const Id*)payload->Data, item.folder);
                }

                ImGui::EndDragDropTarget();
            }

            if (ImGui::BeginPopupContextItem()) {
                if (ImGui::BeginMenu(tr(ICON_MD_DRIVE_FILE_MOVE " Move to"))) {
                    if (ImGui::MenuItem(tr("(No folder)"), nullptr, item.folder.empty())) {
                        moveSelection(item.id, std::string());
                    }

                    for (const std::string& folder : _project.mediaFolders()) {
                        if (ImGui::MenuItem(folder.c_str(), nullptr, item.folder == folder)) {
                            moveSelection(item.id, folder);
                        }
                    }

                    ImGui::EndMenu();
                }

                if (ImGui::MenuItem(tr(ICON_MD_DELETE " Remove from Project"))) {
                    removeMedia = mediaSelected(item.id) ? _mediaSelection : std::vector<Id>{ item.id };
                }

                ImGui::EndPopup();
            }

            ImGui::TableNextColumn();

            if (state.status == media::MediaStatus::Error || missing) {
                ImGui::TextColored(ImVec4(0.9f, 0.35f, 0.3f, 1.f), tr(ICON_MD_WARNING " Error"));
            }
            else if (!item.probed) {
                ImGui::TextDisabled("...");
            }
            else if (state.status == media::MediaStatus::Decoding) {
                ImGui::TextDisabled("%3.0f %%", state.progress * 100.f);
            }
            else {
                ImGui::TextDisabled("%s", formatTimecode(item.duration, _project.sequence().frameRate).c_str());
            }

            ImGui::PopID();
        };

        // A folder: its subfolders, then its media; the top level shows the
        // media of no known folder too.
        const std::vector<std::string>& allFolders = _project.mediaFolders();

        std::function<void(const std::string&)> drawFolder = [&](const std::string& folder) {
            std::vector<std::string> children;

            for (const std::string& path : allFolders) {
                if (Project::parentFolder(path) == folder) children.push_back(path);
            }

            std::sort(children.begin(), children.end());

            for (const std::string& child : children) {
                ImGui::PushID(child.c_str());
                ImGui::TableNextRow();
                ImGui::TableNextColumn();

                const std::string name = child.substr(child.rfind('/') == std::string::npos ? 0 : child.rfind('/') + 1);
                const bool renaming = _renamingFolder == child;
                const bool selected = _selectedMedia == InvalidId && _selectedFolder == child;
                const ImGuiTreeNodeFlags nodeFlags = ImGuiTreeNodeFlags_SpanAllColumns | ImGuiTreeNodeFlags_OpenOnArrow |
                    ImGuiTreeNodeFlags_OpenOnDoubleClick | ImGuiTreeNodeFlags_DefaultOpen | (selected ? ImGuiTreeNodeFlags_Selected : 0);

                const bool open = ImGui::TreeNodeEx("##folder", nodeFlags, "%s %s", ICON_MD_FOLDER, renaming ? "" : name.c_str());

                if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) {
                    _selectedFolder = child;
                    _selectedMedia = InvalidId;
                }

                // Media dropped on it.
                if (ImGui::BeginDragDropTarget()) {
                    if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(TimelineView::MediaPayload)) {
                        moveSelection(*(const Id*)payload->Data, child);
                    }

                    ImGui::EndDragDropTarget();
                }

                if (ImGui::BeginPopupContextItem()) {
                    if (ImGui::MenuItem(tr(ICON_MD_CREATE_NEW_FOLDER " New Folder Inside"))) {
                        newFolderIn = child;
                    }

                    if (ImGui::MenuItem(tr(ICON_MD_EDIT " Rename"))) {
                        _renamingFolder = child;
                        _folderName = name;
                        _focusFolderName = true;
                    }

                    if (ImGui::MenuItem(tr(ICON_MD_DELETE " Delete Folder"), "Del")) {
                        removeFolder = child;
                    }

                    ImGui::EndPopup();
                }

                // Renamed in place: Enter confirms, Escape cancels.
                if (renaming) {
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(-FLT_MIN);

                    if (std::exchange(_focusFolderName, false)) {
                        ImGui::SetKeyboardFocusHere();
                    }

                    const bool entered = ImGui::InputText("##folderName", &_folderName, ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);

                    if (entered || ImGui::IsItemDeactivated()) {
                        if (!ImGui::IsKeyPressed(ImGuiKey_Escape) && _project.renameMediaFolder(child, _folderName)) {
                            const std::string renamed = (Project::parentFolder(child).empty() ? "" : Project::parentFolder(child) + "/") + _folderName;

                            if (_selectedFolder == child) _selectedFolder = renamed;
                            commit("Rename folder");
                        }

                        _renamingFolder.clear();
                    }
                }

                ImGui::TableNextColumn();

                const size_t count = std::count_if(_project.media().begin(), _project.media().end(), [&](const MediaItem& item) {
                    return item.folder == child;
                });

                ImGui::TextDisabled("%zu", count);

                if (open) {
                    drawFolder(child);
                    ImGui::TreePop();
                }

                ImGui::PopID();
            }

            for (const MediaItem& item : _project.media()) {
                const bool known = std::find(allFolders.begin(), allFolders.end(), item.folder) != allFolders.end();

                if (item.folder == folder || (folder.empty() && !known)) {
                    drawItem(item);
                }
            }
        };

        // The sequences of the project, nested by dragging them.
        if (_project.sequences().size() > 1) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();

            if (ImGui::TreeNodeEx("##sequences", ImGuiTreeNodeFlags_SpanAllColumns | ImGuiTreeNodeFlags_DefaultOpen, tr("%s Sequences"), ICON_MD_VIDEO_LIBRARY)) {
                for (const Sequence* sequence : _project.sequences()) {
                    ImGui::PushID((int)sequence->id);
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();

                    const bool current = sequence->id == _project.sequence().id;
                    ImGui::Selectable((std::string(ICON_MD_MOVIE_FILTER " ") + sequence->name + (current ? tr("  (shown)") : "")).c_str(), false, ImGuiSelectableFlags_SpanAllColumns);

                    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                        showSequence(sequence->id);
                    }

                    if (ImGui::BeginDragDropSource()) {
                        ImGui::SetDragDropPayload(TimelineView::SequencePayload, &sequence->id, sizeof(sequence->id));
                        ImGui::Text(ICON_MD_VIDEO_LIBRARY " %s", sequence->name.c_str());
                        ImGui::EndDragDropSource();
                    }

                    ImGui::TableNextColumn();
                    ImGui::TextDisabled("%s", formatTimecode(sequence->duration(), sequence->frameRate).c_str());
                    ImGui::PopID();
                }

                ImGui::TreePop();
            }
        }

        drawFolder("");

        // The rest of the list: out of any folder.
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::Dummy(ImVec2(1.f, std::max(ImGui::GetTextLineHeight(), ImGui::GetContentRegionAvail().y)));

        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(TimelineView::MediaPayload)) {
                moveSelection(*(const Id*)payload->Data, std::string());
            }

            ImGui::EndDragDropTarget();
        }

        // A click in the empty part: nothing selected, or the start of a
        // rectangle selecting the media it crosses.
        if (ImGui::IsItemClicked()) {
            _selectedFolder.clear();
            _selectedMedia = InvalidId;
            _mediaMarquee = true;
            _mediaMarqueeStart = ImGui::GetIO().MousePos;
            _mediaMarqueeBase = ImGui::GetIO().KeyCtrl ? _mediaSelection : std::vector<Id>();
            _mediaSelection = _mediaMarqueeBase;
        }

        if (_mediaMarquee) {
            const ImVec2 mouse = ImGui::GetIO().MousePos;
            const float y0 = std::min(mouse.y, _mediaMarqueeStart.y);
            const float y1 = std::max(mouse.y, _mediaMarqueeStart.y);

            _mediaSelection = _mediaMarqueeBase;

            for (const auto& [id, top, bottom] : itemRows) {
                if (bottom >= y0 && top <= y1 && !mediaSelected(id)) {
                    _mediaSelection.push_back(id);
                }
            }

            ImDrawList* drawList = ImGui::GetForegroundDrawList(ImGui::GetWindowViewport());
            const ImVec2 a(std::min(mouse.x, _mediaMarqueeStart.x), y0);
            const ImVec2 b(std::max(mouse.x, _mediaMarqueeStart.x), y1);
            drawList->AddRectFilled(a, b, IM_COL32(255, 122, 0, 40));
            drawList->AddRect(a, b, IM_COL32(255, 122, 0, 200));

            if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                _mediaMarquee = false;

                if (!_mediaSelection.empty()) {
                    _selectedMedia = _mediaSelection.back();
                }
            }
        }

        ImGui::EndTable();
    }

    // Folders
    if (newFolder) {
        newFolderIn = _selectedFolder.empty() ? std::string("\x01") : _selectedFolder;
    }

    if (!newFolderIn.empty()) {
        const std::string path = _project.addMediaFolder(newFolderIn == "\x01" ? std::string() : newFolderIn, "New Folder");
        _selectedFolder = path;
        _selectedMedia = InvalidId;
        _renamingFolder = path;
        _folderName = path.substr(path.rfind('/') == std::string::npos ? 0 : path.rfind('/') + 1);
        _focusFolderName = true;
        commit("New folder");
    }

    if (!moves.empty()) {
        for (const auto& [mediaId, folder] : moves) {
            _project.moveMediaToFolder(mediaId, folder);
        }

        commit("Move media");
    }

    _mediaPanelFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);

    if (_mediaPanelFocused && _selectedMedia == InvalidId && !_selectedFolder.empty() && _renamingFolder.empty() &&
        !ImGui::GetIO().WantTextInput && ImGui::IsKeyPressed(ImGuiKey_Delete, false)) {
        removeFolder = _selectedFolder;
    }

    if (!removeFolder.empty()) {
        _project.removeMediaFolder(removeFolder);
        _selectedFolder = Project::parentFolder(removeFolder);
        commit("Delete folder");
    }

    // Delete: the selected media, with their clips.
    if (_mediaPanelFocused && !_mediaSelection.empty() && !ImGui::GetIO().WantTextInput &&
        (ImGui::IsKeyPressed(ImGuiKey_Delete, false) || ImGui::IsKeyPressed(ImGuiKey_Backspace, false))) {
        removeMedia = _mediaSelection;
    }

    // Used in the timeline: confirmed first.
    if (!removeMedia.empty()) {
        size_t used = 0;

        for (Id id : removeMedia) {
            used += clipsOfMedia(id).size();
        }

        if (used == 0) {
            removeMediaAndClips(removeMedia);
        }
        else {
            _confirmRemoveMedia = removeMedia;
            ImGui::OpenPopup(tr("Remove Media"));
        }
    }

    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

    if (ImGui::BeginPopupModal(tr("Remove Media"), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        size_t count = 0;

        for (Id id : _confirmRemoveMedia) {
            count += clipsOfMedia(id).size();
        }

        const MediaItem* item = _confirmRemoveMedia.size() == 1 ? _project.findMedia(_confirmRemoveMedia.front()) : nullptr;

        if (_confirmRemoveMedia.empty()) {
            ImGui::CloseCurrentPopup();
        }
        else {
            if (item) {
                ImGui::Text(tr("\"%s\" is used by %zu clip%s of the timeline."), item->name().c_str(), count, count > 1 ? "s" : "");
                ImGui::TextUnformatted(tr("Remove it from the project, with its clips?"));
            }
            else {
                ImGui::Text(tr("These %zu media are used by %zu clip%s of the timeline."), _confirmRemoveMedia.size(), count, count > 1 ? "s" : "");
                ImGui::TextUnformatted(tr("Remove them from the project, with their clips?"));
            }

            ImGui::Spacing();

            if (ImGui::Button(tr(ICON_MD_DELETE " Remove")) || ImGui::IsKeyPressed(ImGuiKey_Enter, false)) {
                ImGui::CloseCurrentPopup();
                removeMediaAndClips(_confirmRemoveMedia);
                _confirmRemoveMedia.clear();
            }

            ImGui::SameLine();

            if (ImGui::Button(tr("Cancel")) || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
                ImGui::CloseCurrentPopup();
                _confirmRemoveMedia.clear();
            }
        }

        ImGui::EndPopup();
    }

    ImGui::End();
}


std::vector<Id> GUI::clipsOfMedia(Id mediaId) const
{
    std::vector<Id> clips;
    const Sequence& sequence = _project.sequence();

    for (const auto* tracks : { &sequence.videoTracks, &sequence.soundTracks, &sequence.audioTracks, &sequence.subtitleTracks }) {
        for (const Track& track : *tracks) {
            for (const Clip& clip : track.clips) {
                if (clip.mediaId == mediaId) clips.push_back(clip.id);
            }
        }
    }

    return clips;
}


void GUI::removeMediaAndClips(const std::vector<Id>& mediaIds)
{
    std::vector<Id> clips;

    for (Id mediaId : mediaIds) {
        const std::vector<Id> ofMedia = clipsOfMedia(mediaId);
        clips.insert(clips.end(), ofMedia.begin(), ofMedia.end());
    }

    _project.removeClips(clips);

    std::erase_if(_project.media(), [&](const MediaItem& item) {
        return std::find(mediaIds.begin(), mediaIds.end(), item.id) != mediaIds.end();
    });

    std::erase_if(_mediaSelection, [&](Id id) { return std::find(mediaIds.begin(), mediaIds.end(), id) != mediaIds.end(); });

    if (std::find(mediaIds.begin(), mediaIds.end(), _selectedMedia) != mediaIds.end()) {
        _selectedMedia = InvalidId;
    }

    std::erase_if(_selection, [&](Id id) { return std::find(clips.begin(), clips.end(), id) != clips.end(); });
    commit("Remove media");
}


void GUI::viewerPanel()
{
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    const bool open = ImGui::Begin(tr("Viewer###Viewer"));
    ImGui::PopStyleVar();

    if (!open) {
        ImGui::End();
        return;
    }

    const Sequence& sequence = _project.sequence();
    const ImGuiStyle& style = ImGui::GetStyle();
    const float transportHeight = ImGui::GetFrameHeight() + 2.f * style.ItemSpacing.y;

    // Frame, letterboxed to the aspect ratio of the sequence.
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const ImVec2 area(avail.x, std::max(1.f, avail.y - transportHeight));

    const float aspect = (float)sequence.width / (float)sequence.height;
    ImVec2 frameSize(area.x, area.x / aspect);

    if (frameSize.y > area.y) {
        frameSize = ImVec2(area.y * aspect, area.y);
    }

    const ImVec2 frameMin(std::floor(origin.x + 0.5f * (area.x - frameSize.x)), std::floor(origin.y + 0.5f * (area.y - frameSize.y)));
    const ImVec2 frameMax(frameMin.x + frameSize.x, frameMin.y + frameSize.y);

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    drawList->AddRectFilled(origin, ImVec2(origin.x + area.x, origin.y + area.y), IM_COL32(12, 12, 12, 255));
    drawList->AddRectFilled(frameMin, frameMax, IM_COL32(0, 0, 0, 255));

    // The composited frame.
    if (_compositor->texture()) {
        drawList->AddImage(_compositor->texture(), frameMin, frameMax);
    }

    viewerInteraction(frameMin, frameSize);

    // Waiting for a picture: a spinner and a word, in the corner. Only after
    // a moment: the decoders often finish a frame or two late while playing,
    // which is not worth a flicker.
    if (!_viewerLoading) {
        _viewerLoadingSince = 0;
    }
    else if (_viewerLoadingSince == 0) {
        _viewerLoadingSince = SDL_GetTicks();
    }

    if (_viewerLoading && SDL_GetTicks() - _viewerLoadingSince > 400) {
        const float unit = ImGui::GetFontSize();
        const char* label = tr("Loading...");
        const ImVec2 textSize = ImGui::CalcTextSize(label);
        const ImVec2 boxMin(frameMin.x + 0.6f * unit, frameMin.y + 0.6f * unit);
        const ImVec2 boxMax(boxMin.x + textSize.x + 2.6f * unit, boxMin.y + textSize.y + 0.8f * unit);

        drawList->AddRectFilled(boxMin, boxMax, IM_COL32(0, 0, 0, 170), 0.4f * unit);

        const ImVec2 center(boxMin.x + 0.9f * unit, 0.5f * (boxMin.y + boxMax.y));
        const float radius = 0.38f * unit;
        const float start = (float)ImGui::GetTime() * 6.f;
        drawList->PathArcTo(center, radius, start, start + 4.2f, 24);
        drawList->PathStroke(IM_COL32(255, 122, 0, 255), 0, 0.14f * unit);

        drawList->AddText(ImVec2(boxMin.x + 1.7f * unit, boxMin.y + 0.4f * unit), IM_COL32(230, 230, 230, 255), label);
    }

    // Transport
    ImGui::SetCursorScreenPos(ImVec2(origin.x + style.ItemSpacing.x, origin.y + area.y + style.ItemSpacing.y));

    const std::string timecode = formatTimecode(_playhead, sequence.frameRate);
    ImGui::TextUnformatted(timecode.c_str());

    const float buttonWidth = ImGui::GetFrameHeight() * 1.3f;
    const float buttonsWidth = 5.f * buttonWidth + 4.f * style.ItemInnerSpacing.x;

    ImGui::SameLine(std::max(0.f, 0.5f * (avail.x - buttonsWidth)));
    ImGui::PushStyleVarX(ImGuiStyleVar_ItemSpacing, style.ItemInnerSpacing.x);

    // Flat, as the other toolbars; Play stands out.
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.f, 0.f, 0.f, 0.f));

    if (ImGui::Button(ICON_MD_SKIP_PREVIOUS, ImVec2(buttonWidth, 0)))   { seek(0); }
    ImGui::SetItemTooltip(tr("Go to Start (Home)"));
    ImGui::SameLine();
    if (ImGui::Button(ICON_MD_NAVIGATE_BEFORE, ImVec2(buttonWidth, 0))) { stepFrames(-1); }
    ImGui::SetItemTooltip(tr("Previous Frame (Left)"));
    ImGui::SameLine();

    ImGui::PushStyleColor(ImGuiCol_Button, style.Colors[ImGuiCol_ButtonActive]);
    if (ImGui::Button(_playing ? ICON_MD_PAUSE : ICON_MD_PLAY_ARROW, ImVec2(buttonWidth, 0))) { togglePlayback(); }
    ImGui::PopStyleColor();
    ImGui::SetItemTooltip(_playing ? tr("Pause (Space)") : tr("Play (Space)"));
    ImGui::SameLine();

    if (ImGui::Button(ICON_MD_NAVIGATE_NEXT, ImVec2(buttonWidth, 0)))   { stepFrames(1); }
    ImGui::SetItemTooltip(tr("Next Frame (Right)"));
    ImGui::SameLine();
    if (ImGui::Button(ICON_MD_SKIP_NEXT, ImVec2(buttonWidth, 0)))       { seek(sequence.duration()); }
    ImGui::SetItemTooltip(tr("Go to End (End)"));

    ImGui::PopStyleColor();
    ImGui::PopStyleVar();

    const std::string duration = formatTimecode(sequence.duration(), sequence.frameRate);
    ImGui::SameLine(std::max(0.f, avail.x - ImGui::CalcTextSize(duration.c_str()).x - style.ItemSpacing.x));
    ImGui::TextDisabled("%s", duration.c_str());

    ImGui::End();
}


void GUI::inspectorPanel()
{
    if (ImGui::Begin(tr("Inspector###Inspector")) && ImGui::BeginTabBar("##inspectorTabs")) {
        if (ImGui::BeginTabItem(tr(ICON_MD_EDIT " Clip"))) {
            clipTab();
            ImGui::EndTabItem();
        }

        // Selected from the effects button of the tracks.
        if (ImGui::BeginTabItem(tr(ICON_MD_TUNE " Effects"), nullptr, std::exchange(_showEffectsTab, false) ? ImGuiTabItemFlags_SetSelected : 0)) {
            effectsTab();
            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
    }

    ImGui::End();
}


void GUI::clipTab()
{
    {
        Inspector::TextSelection textSelection;

        if (_textEditor.active()) {
            textSelection = { _textEditor.clip(), _textEditor.selectionStart(), _textEditor.selectionEnd() };
        }

        const std::string edit = _inspector.draw(_project, _selection, _playhead, *_fonts, textSelection);

        if (!edit.empty()) {
            commit(edit);
            _textEditor.markCommitted(_project);
        }
    }
}


void GUI::viewerInteraction(const ImVec2& frameMin, const ImVec2& frameSize)
{
    const Sequence& sequence = _project.sequence();
    const float toSequence = (float)sequence.width / frameSize.x;
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    const ImVec2 point((mouse.x - frameMin.x) * toSequence, (mouse.y - frameMin.y) * toSequence);

    // The frame is a button: clicks select and drag the layers.
    ImGui::SetCursorScreenPos(frameMin);
    ImGui::InvisibleButton("##frame", frameSize);
    const bool hovered = ImGui::IsItemHovered();

    // Text editing: the editor takes the clicks on its text.
    if (_textEditor.active()) {
        const Clip* clip = _project.findClip(_textEditor.clip());
        const bool visible = clip && _playhead >= clip->start && _playhead < clip->end() &&
            std::find(_selection.begin(), _selection.end(), clip->id) != _selection.end();

        if (visible) {
            // Typed characters come from SDL text input events, which ImGui
            // only enables for its own text fields.
            if (!SDL_TextInputActive(window())) {
                SDL_StartTextInput(window());
            }

            const Time localTime = _playhead - clip->start;
            const float rotation = clip->transform.rotation.at(localTime) * 3.14159265358979f / 180.f;

            ViewerTextEditor::Mapping mapping;
            mapping.scale = clip->transform.scale.at(localTime);
            mapping.cosine = std::cos(rotation);
            mapping.sine = std::sin(rotation);
            mapping.x = 0.5f * sequence.width + clip->transform.x.at(localTime);
            mapping.y = 0.5f * sequence.height + clip->transform.y.at(localTime);
            mapping.frameMin = frameMin;
            mapping.toSequence = toSequence;

            ImGui::GetWindowDrawList()->PushClipRect(frameMin, ImVec2(frameMin.x + frameSize.x, frameMin.y + frameSize.y), true);
            const bool editing = _textEditor.update(_project, *_fonts, mapping, hovered);
            ImGui::GetWindowDrawList()->PopClipRect();

            if (editing) {
                return;
            }
        }

        // A click outside the text, Escape, or the text is no longer there.
        endTextEditing();
    }

    auto inside = [](const std::array<ImVec2, 4>& quad, const ImVec2& p) {
        // Convex quad: same side of the four edges.
        int sign = 0;

        for (int i = 0; i < 4; i++) {
            const ImVec2& a = quad[i];
            const ImVec2& b = quad[(i + 1) % 4];
            const float cross = (b.x - a.x) * (p.y - a.y) - (b.y - a.y) * (p.x - a.x);
            const int s = (cross > 0.f) ? 1 : (cross < 0.f ? -1 : 0);

            if (s != 0 && sign != 0 && s != sign) {
                return false;
            }

            if (s != 0) {
                sign = s;
            }
        }

        return true;
    };

    // Handles of the selected layer (when a single one is selected): the
    // corners scale it, the handle above its top edge rotates it.
    const Compositor::LayerBounds* handleBounds = nullptr;

    if (_selection.size() >= 1) {
        for (const Compositor::LayerBounds& bounds : _compositor->bounds()) {
            // Subtitles are placed by their style, not by handles.
            const Track* track = _project.findTrackOfClip(bounds.clipId);

            if (bounds.clipId == _selection.front() && !(track && track->type == TrackType::Subtitle)) {
                handleBounds = &bounds;
            }
        }
    }

    const float handleRadius = 6.f * toSequence;     // Screen pixels, in sequence pixels
    int hoveredHandle = -1;                          // 0..3: corners, 4: rotation, 5..8: edges (top, right, bottom, left)
    ImVec2 rotationHandle;
    ImVec2 layerCenter;
    ImVec2 edgeHandles[4];

    // A video or image layer can be cropped: its picture, in the pixels of
    // the layer (centered, before its transform), and the transform.
    const Clip* handleClip = handleBounds ? _project.findClip(handleBounds->clipId) : nullptr;
    const MediaItem* handleMedia = (handleClip && handleClip->kind == ClipKind::Media) ? _project.findMedia(handleClip->mediaId) : nullptr;
    const bool croppable = handleMedia && handleMedia->width > 0 && handleMedia->height > 0;
    float pictureHalfWidth = 0.f;
    float pictureHalfHeight = 0.f;

    if (croppable) {
        const float fit = fitScale(handleClip->fit, (float)sequence.width, (float)sequence.height, (float)handleMedia->width, (float)handleMedia->height);
        pictureHalfWidth = 0.5f * (float)handleMedia->width * fit;
        pictureHalfHeight = 0.5f * (float)handleMedia->height * fit;
    }

    // Sequence pixels to the pixels of the layer, and back.
    auto toLayer = [&](const Clip& clip, const ImVec2& p) {
        const Time localTime = std::clamp<Time>(_playhead - clip.start, 0, clip.duration);
        const float angle = clip.transform.rotation.at(localTime) * 3.14159265358979f / 180.f;
        const float scale = std::max(1e-3f, clip.transform.scale.at(localTime));
        const float dx = p.x - (0.5f * sequence.width + clip.transform.x.at(localTime));
        const float dy = p.y - (0.5f * sequence.height + clip.transform.y.at(localTime));

        return ImVec2((dx * std::cos(angle) + dy * std::sin(angle)) / scale, (-dx * std::sin(angle) + dy * std::cos(angle)) / scale);
    };

    auto fromLayer = [&](const Clip& clip, const ImVec2& p) {
        const Time localTime = std::clamp<Time>(_playhead - clip.start, 0, clip.duration);
        const float angle = clip.transform.rotation.at(localTime) * 3.14159265358979f / 180.f;
        const float scale = clip.transform.scale.at(localTime);
        const float sx = p.x * scale;
        const float sy = p.y * scale;

        return ImVec2(0.5f * sequence.width + clip.transform.x.at(localTime) + sx * std::cos(angle) - sy * std::sin(angle),
            0.5f * sequence.height + clip.transform.y.at(localTime) + sx * std::sin(angle) + sy * std::cos(angle));
    };

    if (handleBounds) {
        const auto& c = handleBounds->corners;
        layerCenter = ImVec2(0.25f * (c[0].x + c[1].x + c[2].x + c[3].x), 0.25f * (c[0].y + c[1].y + c[2].y + c[3].y));

        // Above the middle of the top edge, away from the center.
        const ImVec2 top(0.5f * (c[0].x + c[1].x), 0.5f * (c[0].y + c[1].y));
        ImVec2 up(top.x - layerCenter.x, top.y - layerCenter.y);
        const float length = std::max(1.f, std::sqrt(up.x * up.x + up.y * up.y));
        const float offset = 28.f * toSequence;
        rotationHandle = ImVec2(top.x + up.x / length * offset, top.y + up.y / length * offset);

        auto near = [&](const ImVec2& p) {
            return std::abs(point.x - p.x) <= 1.5f * handleRadius && std::abs(point.y - p.y) <= 1.5f * handleRadius;
        };

        for (int i = 0; i < 4; i++) {
            if (near(c[i])) hoveredHandle = i;
        }

        // Middles of the edges, which crop.
        for (int i = 0; i < 4; i++) {
            edgeHandles[i] = ImVec2(0.5f * (c[i].x + c[(i + 1) % 4].x), 0.5f * (c[i].y + c[(i + 1) % 4].y));

            if (croppable && near(edgeHandles[i])) hoveredHandle = 5 + i;
        }

        if (near(rotationHandle)) {
            hoveredHandle = 4;
        }
    }

    const bool cropKey = ImGui::GetIO().KeyAlt && croppable;

    if (hovered && hoveredHandle >= 0 && _draggedLayer == InvalidId) {
        ImGui::SetMouseCursor(hoveredHandle == 4 ? ImGuiMouseCursor_Hand
            : (hoveredHandle == 5 || hoveredHandle == 7) ? ImGuiMouseCursor_ResizeNS
            : (hoveredHandle == 6 || hoveredHandle == 8) ? ImGuiMouseCursor_ResizeEW
            : ImGuiMouseCursor_ResizeNWSE);
    }

    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && hoveredHandle >= 0) {
        const Clip* clip = _project.findClip(handleBounds->clipId);
        const Time localTime = _playhead - clip->start;

        _draggedLayer = clip->id;
        _layerDrag = (hoveredHandle == 4) ? LayerDrag::Rotate
            : (hoveredHandle >= 5 || cropKey) ? LayerDrag::Crop : LayerDrag::Scale;

        // The sides of the handle: corners top left, top right, bottom
        // right, bottom left; edges top, right, bottom, left.
        _cropLeft = hoveredHandle == 0 || hoveredHandle == 3 || hoveredHandle == 8;
        _cropRight = hoveredHandle == 1 || hoveredHandle == 2 || hoveredHandle == 6;
        _cropTop = hoveredHandle == 0 || hoveredHandle == 1 || hoveredHandle == 5;
        _cropBottom = hoveredHandle == 2 || hoveredHandle == 3 || hoveredHandle == 7;
        _dragStartMouse = point;
        _dragCenter = layerCenter;
        _dragStartScale = clip->transform.scale.at(localTime);
        _dragStartRotation = clip->transform.rotation.at(localTime);
        _dragMoved = false;
        stopPlayback();
    }
    else if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        // The top layer under the mouse; the selected one first, so that a
        // layer under another can still be dragged once selected.
        Id picked = InvalidId;

        for (const Compositor::LayerBounds& bounds : _compositor->bounds()) {
            if (inside(bounds.corners, point) && std::find(_selection.begin(), _selection.end(), bounds.clipId) != _selection.end()) {
                picked = bounds.clipId;
            }
        }

        for (const Compositor::LayerBounds& bounds : _compositor->bounds()) {
            if (picked == InvalidId && inside(bounds.corners, point)) {
                picked = bounds.clipId;
            }
        }

        // Double click on a text: edit it.
        const Clip* pickedClip = (picked != InvalidId) ? _project.findClip(picked) : nullptr;

        if (pickedClip && pickedClip->kind == ClipKind::Text && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            beginTextEditing(picked);
            return;
        }

        if (picked != InvalidId) {
            if (std::find(_selection.begin(), _selection.end(), picked) == _selection.end()) {
                _selection = { picked };
            }

            const Clip* clip = _project.findClip(picked);
            const Time localTime = _playhead - clip->start;
            const Track* track = _project.findTrackOfClip(picked);

            // A subtitle: selected, to edit its text, but not moved.
            if (track && track->type == TrackType::Subtitle) {
                return;
            }

            _draggedLayer = picked;
            _layerDrag = LayerDrag::Move;
            _dragStartMouse = point;
            _dragStartX = clip->transform.x.at(localTime);
            _dragStartY = clip->transform.y.at(localTime);
            _dragMoved = false;
            stopPlayback();
        }
        else {
            _selection.clear();
        }
    }

    if (_draggedLayer != InvalidId) {
        Clip* clip = _project.findClip(_draggedLayer);

        if (!clip || !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            if (clip && _dragMoved) {
                commit(_layerDrag == LayerDrag::Move ? "Move layer"
                    : _layerDrag == LayerDrag::Scale ? "Scale layer"
                    : _layerDrag == LayerDrag::Crop ? "Crop" : "Rotate layer");
            }

            _draggedLayer = InvalidId;
        }
        else if (_layerDrag == LayerDrag::Scale) {
            // Uniform scale: the distance to the center, relative to the
            // one when the drag started.
            const Time localTime = std::clamp<Time>(_playhead - clip->start, 0, clip->duration);
            const float startDistance = std::max(1.f, std::hypot(_dragStartMouse.x - _dragCenter.x, _dragStartMouse.y - _dragCenter.y));
            const float distance = std::hypot(point.x - _dragCenter.x, point.y - _dragCenter.y);
            float scale = std::max(0.01f, _dragStartScale * distance / startDistance);

            // Snaps to 100 %, Ctrl to scale freely (Alt crops).
            if (!ImGui::GetIO().KeyCtrl && std::abs(scale - 1.f) < 0.03f) {
                scale = 1.f;
            }

            clip->transform.scale.set(localTime, scale);
            _dragMoved = true;
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNWSE);
        }
        else if (_layerDrag == LayerDrag::Crop) {
            // The side follows the mouse, in the picture; the rest of it
            // stays in place. Snaps to the edges and the middle, Ctrl to
            // crop freely.
            if (croppable && handleClip && handleClip->id == clip->id) {
                const ImVec2 local = toLayer(*clip, point);
                const float width = 2.f * pictureHalfWidth;
                const float height = 2.f * pictureHalfHeight;
                const float snap = 8.f * toSequence / std::max(1e-3f, clip->transform.scale.at(std::clamp<Time>(_playhead - clip->start, 0, clip->duration)));
                Frame& frame = clip->frame;

                auto snapped = [&](float fraction, float size) {
                    if (!ImGui::GetIO().KeyCtrl) {
                        for (float to : { 0.f, 0.5f }) {
                            if (std::abs(fraction - to) * size < snap) return to;
                        }
                    }

                    return fraction;
                };

                if (_cropLeft) {
                    frame.cropLeft = std::clamp(snapped((local.x + pictureHalfWidth) / width, width), 0.f, std::max(0.f, 0.99f - frame.cropRight));
                }

                if (_cropRight) {
                    frame.cropRight = std::clamp(snapped((pictureHalfWidth - local.x) / width, width), 0.f, std::max(0.f, 0.99f - frame.cropLeft));
                }

                if (_cropTop) {
                    frame.cropTop = std::clamp(snapped((local.y + pictureHalfHeight) / height, height), 0.f, std::max(0.f, 0.99f - frame.cropBottom));
                }

                if (_cropBottom) {
                    frame.cropBottom = std::clamp(snapped((pictureHalfHeight - local.y) / height, height), 0.f, std::max(0.f, 0.99f - frame.cropTop));
                }

                _dragMoved = true;
            }

            ImGui::SetMouseCursor((_cropLeft || _cropRight) && (_cropTop || _cropBottom) ? ImGuiMouseCursor_ResizeNWSE
                : (_cropLeft || _cropRight) ? ImGuiMouseCursor_ResizeEW : ImGuiMouseCursor_ResizeNS);
        }
        else if (_layerDrag == LayerDrag::Rotate) {
            const Time localTime = std::clamp<Time>(_playhead - clip->start, 0, clip->duration);
            const float startAngle = std::atan2(_dragStartMouse.y - _dragCenter.y, _dragStartMouse.x - _dragCenter.x);
            const float angle = std::atan2(point.y - _dragCenter.y, point.x - _dragCenter.x);
            float rotation = _dragStartRotation + (angle - startAngle) * 180.f / 3.14159265358979f;

            // Shift: steps of 15 degrees.
            if (ImGui::GetIO().KeyShift) {
                rotation = std::round(rotation / 15.f) * 15.f;
            }

            clip->transform.rotation.set(localTime, rotation);
            _dragMoved = true;
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        }
        else {
            const float dx = point.x - _dragStartMouse.x;
            const float dy = point.y - _dragStartMouse.y;

            if (std::abs(dx) > 1.f || std::abs(dy) > 1.f || _dragMoved) {
                const Time localTime = std::clamp<Time>(_playhead - clip->start, 0, clip->duration);
                float x = _dragStartX + dx;
                float y = _dragStartY + dy;

                // Snaps to the center of the frame, Alt to move freely.
                if (!ImGui::GetIO().KeyAlt) {
                    const float snap = 8.f * toSequence;
                    if (std::abs(x) < snap) x = 0.f;
                    if (std::abs(y) < snap) y = 0.f;
                }

                clip->transform.x.set(localTime, std::round(x));
                clip->transform.y.set(localTime, std::round(y));
                _dragMoved = true;
                ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
            }
        }
    }

    // Outline of the selected layers.
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    drawList->PushClipRect(frameMin, ImVec2(frameMin.x + frameSize.x, frameMin.y + frameSize.y), true);

    for (const Compositor::LayerBounds& bounds : _compositor->bounds()) {
        if (std::find(_selection.begin(), _selection.end(), bounds.clipId) == _selection.end()) {
            continue;
        }

        ImVec2 corners[4];

        for (int i = 0; i < 4; i++) {
            corners[i] = ImVec2(frameMin.x + bounds.corners[i].x / toSequence, frameMin.y + bounds.corners[i].y / toSequence);
        }

        drawList->AddPolyline(corners, 4, IM_COL32(255, 122, 0, 255), ImDrawFlags_Closed, 1.5f);

        if (&bounds != handleBounds) {
            continue;
        }

        // While cropping: the whole picture, faintly.
        const bool cropping = _draggedLayer == bounds.clipId && _layerDrag == LayerDrag::Crop;

        if (croppable && (cropping || cropKey) && handleClip) {
            ImVec2 whole[4];
            const ImVec2 local[4] = {
                { -pictureHalfWidth, -pictureHalfHeight }, { pictureHalfWidth, -pictureHalfHeight },
                { pictureHalfWidth, pictureHalfHeight }, { -pictureHalfWidth, pictureHalfHeight } };

            for (int i = 0; i < 4; i++) {
                const ImVec2 p = fromLayer(*handleClip, local[i]);
                whole[i] = ImVec2(frameMin.x + p.x / toSequence, frameMin.y + p.y / toSequence);
            }

            drawList->AddPolyline(whole, 4, IM_COL32(120, 200, 255, 150), ImDrawFlags_Closed, 1.f);
        }

        // Handles: crop ones in blue (edges, and the corners with Alt).
        const float size = handleRadius / toSequence;
        const ImU32 cropColor = IM_COL32(60, 170, 255, 255);

        for (int i = 0; i < 4; i++) {
            const ImU32 outline = cropKey ? cropColor : IM_COL32(255, 122, 0, 255);
            const ImU32 fill = (hoveredHandle == i) ? outline : IM_COL32(255, 255, 255, 255);
            drawList->AddRectFilled(ImVec2(corners[i].x - size, corners[i].y - size), ImVec2(corners[i].x + size, corners[i].y + size), fill);
            drawList->AddRect(ImVec2(corners[i].x - size, corners[i].y - size), ImVec2(corners[i].x + size, corners[i].y + size), outline);
        }

        if (croppable) {
            const float edge = 0.75f * size;

            for (int i = 0; i < 4; i++) {
                const ImVec2 e(frameMin.x + edgeHandles[i].x / toSequence, frameMin.y + edgeHandles[i].y / toSequence);
                const ImU32 fill = (hoveredHandle == 5 + i) ? cropColor : IM_COL32(255, 255, 255, 255);
                drawList->AddRectFilled(ImVec2(e.x - edge, e.y - edge), ImVec2(e.x + edge, e.y + edge), fill);
                drawList->AddRect(ImVec2(e.x - edge, e.y - edge), ImVec2(e.x + edge, e.y + edge), cropColor);
            }
        }

        const ImVec2 rotation(frameMin.x + rotationHandle.x / toSequence, frameMin.y + rotationHandle.y / toSequence);
        const ImVec2 top(0.5f * (corners[0].x + corners[1].x), 0.5f * (corners[0].y + corners[1].y));

        drawList->AddLine(top, rotation, IM_COL32(255, 122, 0, 255), 1.5f);
        drawList->AddCircleFilled(rotation, size * 1.1f, (hoveredHandle == 4) ? IM_COL32(255, 122, 0, 255) : IM_COL32(255, 255, 255, 255));
        drawList->AddCircle(rotation, size * 1.1f, IM_COL32(255, 122, 0, 255), 0, 1.5f);
    }

    drawList->PopClipRect();
}


void GUI::beginTextEditing(Id clipId)
{
    const Clip* clip = _project.findClip(clipId);

    if (!clip || clip->kind != ClipKind::Text) {
        return;
    }

    endTextEditing();
    stopPlayback();

    // The text must be visible: the playhead goes into it.
    if (_playhead < clip->start || _playhead >= clip->end()) {
        seek(clip->start);
    }

    _selection = { clipId };
    _textEditor.begin(*clip);

    // Arrows move the caret, not the ImGui focus.
    ImGui::GetIO().ConfigFlags &= ~ImGuiConfigFlags_NavEnableKeyboard;
}


void GUI::endTextEditing()
{
    if (!_textEditor.active()) {
        return;
    }

    if (_textEditor.end(_project)) {
        commit("Edit text");
    }

    SDL_StopTextInput(window());

    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
}


void GUI::addText()
{
    Sequence& sequence = _project.sequence();
    const Time start = snapToFrame(_playhead, sequence.frameRate);
    const Time duration = 5 * TicksPerSecond;

    // On the top video track with room at the playhead, or on a new track.
    Id clipId = InvalidId;

    for (int i = (int)sequence.videoTracks.size() - 1; i >= 0 && clipId == InvalidId; i--) {
        clipId = _project.addTextClip(sequence.videoTracks[i].id, start, duration, tr("Your text"));
    }

    if (clipId == InvalidId) {
        addTrack(TrackType::Video);
        clipId = _project.addTextClip(sequence.videoTracks.back().id, start, duration, tr("Your text"));
    }

    if (clipId != InvalidId) {
        _selection = { clipId };
        commit("Add text");

        // Typing replaces the placeholder.
        beginTextEditing(clipId);
    }
}


std::filesystem::path GUI::shaderFolder() const
{
    const char* documents = SDL_GetUserFolder(SDL_FOLDER_DOCUMENTS);
    return (documents ? std::filesystem::u8path(documents) : std::filesystem::temp_directory_path()) / "QuickEdit" / "Shaders";
}


void GUI::reloadShaders(bool commitChanges)
{
    _shaderReloadPending = true;
    _shaderReloadCommit = _shaderReloadCommit || commitChanges;
}


void GUI::applyShaderReload()
{
    const bool commitChanges = std::exchange(_shaderReloadCommit, false);
    _shaderReloadPending = false;
    _userShaders = loadShaderFolder(shaderFolder());

    // The copies in the project follow their files.
    bool changed = false;

    for (const ShaderDef& shader : _userShaders) {
        const ShaderDef* stored = nullptr;

        for (const ShaderDef& s : _project.shaders()) {
            if (s.id == shader.id) stored = &s;
        }

        if (stored && stored->source != shader.source) {
            _project.storeShader(shader);
            changed = true;
        }
    }

    if (changed && commitChanges) {
        commit("Reload shaders");
    }
}


void GUI::openShaderFolder()
{
    const std::filesystem::path folder = shaderFolder();
    std::error_code ec;

    // Two examples to start from, the first time.
    if (!std::filesystem::exists(folder, ec)) {
        std::filesystem::create_directories(folder, ec);

        auto write = [&](const char* name, const char* text) {
            std::ofstream(folder / name, std::ios::binary) << text;
        };

        write("Rings.glsl",
            "// A generator: the picture of a clip, computed for each of its pixels.\n"
            "// uv: position in the picture (0 to 1), time: seconds since the start of the clip.\n"
            "// iResolution: size in pixels, iDuration: length of the clip, param(i): the parameters below.\n"
            "// Edit > Add Generator > Edit Shader: saved with Ctrl + S, and used at once.\n"
            "// @param Speed 0 5 1\n"
            "// @param Rings 1 40 12\n"
            "vec4 generate(vec2 uv, float time)\n"
            "{\n"
            "    vec2 p = (uv - 0.5) * vec2(iResolution.x / iResolution.y, 1.0);\n"
            "    float v = 0.5 + 0.5 * sin(length(p) * param(1) * 6.2832 - time * param(0) * 6.2832);\n"
            "    return vec4(v * vec3(0.2, 0.6, 1.0), 1.0);\n"
            "}\n");

        write("Stripes.glsl",
            "// A transition: mixes the outgoing and the incoming pictures.\n"
            "// getFromColor(uv), getToColor(uv): the two pictures; progress: 0 to 1; ratio: width / height.\n"
            "vec4 transition(vec2 uv)\n"
            "{\n"
            "    float shown = step(fract(uv.x * 10.0), progress);\n"
            "    return mix(getFromColor(uv), getToColor(uv), shown);\n"
            "}\n");

        reloadShaders(false);
    }

    const std::u8string path = folder.generic_u8string();
    SDL_OpenURL(("file:///" + std::string(path.begin(), path.end())).c_str());
}


namespace {

const char* const GeneratorExample =
    "// A generator: the picture of a clip, computed for each of its pixels.\n"
    "//   uv: position in the picture (0 to 1, from the top left)\n"
    "//   time: seconds since the start of the clip, iDuration: its length\n"
    "//   iResolution: size of the picture in pixels\n"
    "//   param(i): the parameters declared below (up to 8), animated in the Inspector\n"
    "// Saved with Ctrl + S, and used at once.\n"
    "//\n"
    "// @param Speed 0 5 1\n"
    "// @param Scale 1 20 6\n"
    "// @param Hue 0 1 0.6\n"
    "\n"
    "vec3 palette(float t)\n"
    "{\n"
    "    return 0.5 + 0.5 * cos(6.2832 * (t + vec3(0.0, 0.33, 0.67)));\n"
    "}\n"
    "\n"
    "vec4 generate(vec2 uv, float time)\n"
    "{\n"
    "    vec2 p = (uv - 0.5) * vec2(iResolution.x / iResolution.y, 1.0) * param(1);\n"
    "    float t = time * param(0);\n"
    "    float v = sin(p.x + t) + sin(p.y * 1.3 - t) + sin(length(p) * 1.5 - t * 1.7);\n"
    "    return vec4(palette(v * 0.15 + param(2)), 1.0);\n"
    "}\n";

const char* const TransitionExample =
    "// A transition: mixes the outgoing and the incoming pictures.\n"
    "//   uv: position in the picture (0 to 1, from the top left)\n"
    "//   getFromColor(uv): the outgoing picture, getToColor(uv): the incoming one\n"
    "//   progress: 0 to 1 during the transition, ratio: width / height\n"
    "// Saved with Ctrl + S, and used at once.\n"
    "\n"
    "vec4 transition(vec2 uv)\n"
    "{\n"
    "    // A diagonal wipe with a soft edge.\n"
    "    float edge = (uv.x + uv.y) * 0.5;\n"
    "    float shown = smoothstep(edge - 0.1, edge + 0.1, progress * 1.2 - 0.1);\n"
    "    return mix(getFromColor(uv), getToColor(uv), shown);\n"
    "}\n";

std::string readText(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary);
    std::stringstream text;
    text << file.rdbuf();
    return text.str();
}

} // namespace


std::optional<ShaderDef> GUI::newShader(ShaderKind kind)
{
    const std::filesystem::path folder = shaderFolder();
    std::error_code ec;
    std::filesystem::create_directories(folder, ec);

    // A free name: "My Generator", "My Generator 2"...
    const std::string base = (kind == ShaderKind::Generator) ? "My Generator" : "My Transition";
    std::string name = base;

    for (int i = 2; std::filesystem::exists(folder / std::filesystem::u8path(name + ".glsl"), ec); i++) {
        name = base + " " + std::to_string(i);
    }

    const std::filesystem::path path = folder / std::filesystem::u8path(name + ".glsl");

    if (!(std::ofstream(path, std::ios::binary) << (kind == ShaderKind::Generator ? GeneratorExample : TransitionExample))) {
        SDL_Log("Could not write %s", path.string().c_str());
        return std::nullopt;
    }

    reloadShaders(false);
    editShader("file:" + name);

    // A copy: the list of the shaders changes at the next frame.
    for (const ShaderDef& shader : loadShaderFolder(folder)) {
        if (shader.id == "file:" + name) return shader;
    }

    return std::nullopt;
}


void GUI::editShader(const std::string& shaderId)
{
    if (shaderId.rfind("file:", 0) != 0) {
        return;
    }

    // Unsaved changes of another shader are kept in its file first.
    const std::filesystem::path path = shaderFolder() / std::filesystem::u8path(shaderId.substr(5) + ".glsl");

    if (_showShaderEditor && path != _shaderEditorPath && _shaderEditorText != _shaderEditorSaved) {
        saveShader();
    }

    // Not in the folder (a project from another computer): its copy in the
    // project is written there.
    std::error_code ec;

    if (!std::filesystem::exists(path, ec)) {
        const ShaderDef* stored = _project.findShader(shaderId);

        if (!stored) {
            return;
        }

        std::filesystem::create_directories(path.parent_path(), ec);
        std::ofstream(path, std::ios::binary) << stored->source;
        reloadShaders(false);
    }

    _shaderEditorPath = path;
    _shaderEditorText = readText(path);
    _shaderEditorSaved = _shaderEditorText;
    _showShaderEditor = true;
    setCodeEditorText(_shaderEditorText);
    checkEditedShader();
    ImGui::SetWindowFocus("###shaderEditor");
}


void GUI::checkEditedShader()
{
    ShaderDef shader;
    const std::u8string stem = _shaderEditorPath.stem().u8string();
    shader.name = std::string(stem.begin(), stem.end());

    if (!parseShader(_shaderEditorText, shader)) {
        _shaderEditorError = tr("Define vec4 generate(vec2 uv, float time) for a generator, or vec4 transition(vec2 uv) for a transition.");
    }
    else {
        _shaderEditorError = Compositor::checkShader(shader);
    }

    _shaderEditorDirty = false;
    _shaderEditorChecked = ImGui::GetTime();
    markShaderErrors();
}


void GUI::setCodeEditorText(const std::string& text)
{
    if (!_codeEditor) {
        _codeEditor = std::make_unique<TextEditor>();
        _codeEditor->SetLanguage(TextEditor::Language::Glsl());
        _codeEditor->SetPalette(TextEditor::GetDarkPalette());
        _codeEditor->SetTabSize(4);
        _codeEditor->SetShowLineNumbersEnabled(true);
        _codeEditor->SetShowWhitespacesEnabled(false);
    }

    _codeEditor->SetText(text);
    _codeEditorUndoIndex = _codeEditor->GetUndoIndex();
}


void GUI::markShaderErrors()
{
    if (!_codeEditor) {
        return;
    }

    _codeEditor->ClearMarkers();

    // The messages of the compiler: "name:line: error: text", the lines of
    // the file from 1 (the wrapping code says #line 1).
    std::istringstream messages(_shaderEditorError);
    std::string message;

    while (std::getline(messages, message)) {
        const size_t colon = message.find(':');
        const size_t next = (colon == std::string::npos) ? std::string::npos : message.find(':', colon + 1);

        if (next == std::string::npos) {
            continue;
        }

        const std::string number = message.substr(colon + 1, next - colon - 1);

        if (number.empty() || !std::all_of(number.begin(), number.end(), [](char c) { return c >= '0' && c <= '9'; })) {
            continue;
        }

        const size_t line = (size_t)std::max(1, std::atoi(number.c_str())) - 1;
        const std::string text = message.substr(std::min(message.size(), next + 2));
        const bool warning = text.rfind("warning", 0) == 0;
        const ImU32 color = warning ? IM_COL32(230, 170, 40, 90) : IM_COL32(220, 60, 50, 90);

        if (line < _codeEditor->GetLineCount()) {
            _codeEditor->AddMarker(line, color, color, text, text);
        }
    }
}


void GUI::saveShader()
{
    if (_shaderEditorPath.empty()) {
        return;
    }

    if (!(std::ofstream(_shaderEditorPath, std::ios::binary) << _shaderEditorText)) {
        _shaderEditorError = "Could not write " + _shaderEditorPath.string();
        return;
    }

    _shaderEditorSaved = _shaderEditorText;
    checkEditedShader();

    // The clips using it follow.
    reloadShaders(true);
}


void GUI::shaderEditorWindow()
{
    _shaderEditorFocused = false;

    if (!_testEditShader.empty()) {
        editShader("file:" + std::exchange(_testEditShader, std::string()));
    }

    if (!_showShaderEditor) {
        return;
    }

    const std::u8string stem = _shaderEditorPath.stem().u8string();
    const std::string name(stem.begin(), stem.end());
    const bool modified = _shaderEditorText != _shaderEditorSaved;
    const std::string title = std::string(ICON_MD_CODE " ") + name + (modified ? " *" : "") + "###shaderEditor";

    ImGui::SetNextWindowSize(ImVec2(44.f * ImGui::GetFontSize(), 32.f * ImGui::GetFontSize()), ImGuiCond_FirstUseEver);

    bool open = true;

    if (!ImGui::Begin(title.c_str(), &open, ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }

    _shaderEditorFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);

    // Closed: what was typed is kept.
    if (!open) {
        if (modified) saveShader();
        _showShaderEditor = false;
    }

    // Toolbar, flat as those of the other panels.
    ImGui::PushStyleVarX(ImGuiStyleVar_ItemSpacing, ImGui::GetStyle().ItemInnerSpacing.x);

    if (panelButton(ICON_MD_SAVE " Save", "Saved, and used by the clips at once (Ctrl+S)", false, modified)) {
        saveShader();
    }

    if (panelButton(ICON_MD_OPEN_IN_NEW " Open in External Editor", nullptr)) {
        if (modified) saveShader();
        const std::u8string path = _shaderEditorPath.generic_u8string();
        SDL_OpenURL(("file:///" + std::string(path.begin(), path.end())).c_str());
    }

    if (panelButton(ICON_MD_REFRESH " Reload", "Read the file again (after a change in another editor)")) {
        _shaderEditorText = readText(_shaderEditorPath);
        _shaderEditorSaved = _shaderEditorText;
        setCodeEditorText(_shaderEditorText);
        checkEditedShader();
        reloadShaders(true);
    }

    if (panelButton(ICON_MD_FOLDER_OPEN " Folder", "Open the folder of the shaders")) {
        openShaderFolder();
    }

    ImGui::NewLine();
    ImGui::PopStyleVar();

    // The code, with the messages of the compiler below.
    const float errorHeight = _shaderEditorError.empty() ? ImGui::GetFrameHeightWithSpacing()
                                                         : 5.f * ImGui::GetTextLineHeightWithSpacing();

    if (!_codeEditor) {
        setCodeEditorText(_shaderEditorText);
    }

    if (_codeFont) {
        ImGui::PushFont(_codeFont, 0.f);
    }

    _codeEditor->Render("##code", ImVec2(-FLT_MIN, -errorHeight), ImGuiChildFlags_Borders);

    if (_codeFont) {
        ImGui::PopFont();
    }

    // Typed (or undone): the text follows, checked once typing pauses.
    if (_codeEditor->GetUndoIndex() != _codeEditorUndoIndex) {
        _codeEditorUndoIndex = _codeEditor->GetUndoIndex();
        _shaderEditorText = _codeEditor->GetText();
        _shaderEditorDirty = true;
        _shaderEditorChecked = ImGui::GetTime();
    }

    // Compiled once typing pauses.
    if (_shaderEditorDirty && ImGui::GetTime() - _shaderEditorChecked > 0.4) {
        checkEditedShader();
    }

    if (_shaderEditorError.empty()) {
        ImGui::TextColored(ImVec4(0.4f, 0.8f, 0.4f, 1.f), tr(ICON_MD_CHECK " Compiles.%s"), modified ? " Ctrl + S to use it." : "");
    }
    else {
        ImGui::BeginChild("##errors", ImVec2(-FLT_MIN, -FLT_MIN));
        ImGui::PushTextWrapPos(0.f);
        ImGui::TextColored(ImVec4(0.9f, 0.35f, 0.3f, 1.f), ICON_MD_WARNING " %s", _shaderEditorError.c_str());
        ImGui::PopTextWrapPos();
        ImGui::EndChild();
    }

    ImGui::End();
}


void GUI::addGenerator(const ShaderDef& shader)
{
    Sequence& sequence = _project.sequence();
    const Time start = snapToFrame(_playhead, sequence.frameRate);
    const Time duration = 5 * TicksPerSecond;

    // On the top video track with room at the playhead, or on a new track.
    Id clipId = InvalidId;

    for (int i = (int)sequence.videoTracks.size() - 1; i >= 0 && clipId == InvalidId; i--) {
        clipId = _project.addGeneratorClip(sequence.videoTracks[i].id, start, duration, shader);
    }

    if (clipId == InvalidId) {
        addTrack(TrackType::Video);
        clipId = _project.addGeneratorClip(sequence.videoTracks.back().id, start, duration, shader);
    }

    if (clipId != InvalidId) {
        _selection = { clipId };
        commit("Add generator");
    }
}


void GUI::addSubtitle()
{
    Sequence& sequence = _project.sequence();
    const Time start = snapToFrame(_playhead, sequence.frameRate);
    const Time duration = 3 * TicksPerSecond;

    // On the first subtitle track with room, or on a new one.
    Id clipId = InvalidId;

    for (size_t i = 0; i < sequence.subtitleTracks.size() && clipId == InvalidId; i++) {
        clipId = _project.addTextClip(sequence.subtitleTracks[i].id, start, duration, tr("Subtitle"));
    }

    if (clipId == InvalidId) {
        sequence.addTrack(TrackType::Subtitle, _project.newId(), InvalidId);
        clipId = _project.addTextClip(sequence.subtitleTracks.back().id, start, duration, tr("Subtitle"));
    }

    if (clipId != InvalidId) {
        _selection = { clipId };
        commit("Add subtitle");
    }
}


std::vector<float> GUI::speechSamples(std::string& error)
{
    const Sequence& sequence = _project.sequence();
    const uint32_t rate = sequence.sampleRate;

    // The sounds must be decoded.
    for (const auto* tracks : { &sequence.soundTracks, &sequence.audioTracks }) {
        for (const Track& track : *tracks) {
            for (const Clip& clip : track.clips) {
                if (!track.muted && !_media.audio(clip.mediaId, clip.audioStream)) {
                    error = "Some sounds are still being analyzed: try again in a moment.";
                    return {};
                }
            }
        }
    }

    // The mix, without effects (clearer speech), in mono at 16 kHz: of a
    // track alone when one is chosen (the others muted in a copy).
    Project source = _project;

    if (_subtitleSourceTrack != InvalidId && source.findTrack(_subtitleSourceTrack)) {
        for (auto* tracks : { &source.sequence().soundTracks, &source.sequence().audioTracks }) {
            for (Track& track : *tracks) {
                track.muted = track.id != _subtitleSourceTrack;
                track.solo = false;
            }
        }
    }

    const auto graph = audio::buildMixGraph(source, [this](Id mediaId, int stream) { return _media.audio(mediaId, stream); });
    const int64_t frames = mulDivFloor(sequence.duration(), rate, TicksPerSecond);

    if (frames <= 0) {
        error = "The sequence has no sound.";
        return {};
    }

    std::vector<float> mono((size_t)frames);
    std::vector<float> block(2 * 48000);

    for (int64_t first = 0; first < frames; first += 48000) {
        const int64_t count = std::min<int64_t>(48000, frames - first);
        audio::mix(*graph, first, count, block.data());

        for (int64_t i = 0; i < count; i++) {
            mono[(size_t)(first + i)] = 0.5f * (block[2 * i] + block[2 * i + 1]);
        }
    }

    const double step = (double)rate / 16000.0;
    std::vector<float> samples((size_t)((double)frames / step));

    for (size_t i = 0; i < samples.size(); i++) {
        const double at = (double)i * step;
        const size_t index = (size_t)at;
        const float next = (index + 1 < mono.size()) ? mono[index + 1] : mono[index];
        samples[i] = mono[index] + (next - mono[index]) * (float)(at - (double)index);
    }

    return samples;
}


void GUI::subtitleGeneratorWindow()
{
    // Generated: the subtitles on a track of their own.
    if (_subtitleGenerator.done()) {
        const std::vector<SubtitleCue> cues = _subtitleGenerator.cues();
        _generatorError = _subtitleGenerator.error();
        _subtitleGenerator.acknowledge();

        if (_testTranscribe) {
            SDL_Log("Transcription: %zu cue(s) %s", cues.size(), _generatorError.c_str());

            for (const SubtitleCue& cue : cues) {
                SDL_Log("  %.2f - %.2f  %s", timeToSeconds(cue.start), timeToSeconds(cue.end), cue.text.c_str());
            }

            _testQuitTicks = SDL_GetTicks() + 3000;
        }

        if (!cues.empty()) {
            Sequence& sequence = _project.sequence();
            const Id trackId = _project.newId();
            sequence.addTrack(TrackType::Subtitle, trackId, InvalidId);
            sequence.subtitleTracks.back().name = "Generated";

            for (const SubtitleCue& cue : cues) {
                _project.addTextClip(trackId, cue.start, cue.end - cue.start, cue.text);
            }

            commit("Generate subtitles");
        }
        else if (_generatorError.empty()) {
            _generatorError = tr("No speech was recognized.");
        }
    }

    if (!_showSubtitleGenerator) {
        return;
    }

    ImGui::SetNextWindowSize(ImVec2(30.f * ImGui::GetFontSize(), 0.f), ImGuiCond_Appearing);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

    if (!ImGui::Begin(tr(ICON_MD_RECORD_VOICE_OVER " Generate Subtitles###subtitleGenerator"), &_showSubtitleGenerator, ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }

    if (!speechRecognitionAvailable()) {
        ImGui::TextWrapped(tr("This build has no speech recognition: whisper.cpp was not found when it was built (see docs/ARCHITECTURE.md)."));
        ImGui::End();
        return;
    }

    const bool busy = _subtitleGenerator.running();

    // The model: the most accurate downloaded one by default.
    if (_whisperModel.empty()) {
        for (const WhisperModel& model : whisperModels()) {
            if (modelDownloaded(model)) _whisperModel = model.id;
        }
    }

    ImGui::SeparatorText(tr("Model"));
    ImGui::TextDisabled(tr("The larger, the more accurate; the large ones want a GPU."));
    ImGui::BeginDisabled(busy);

    // Columns: after the longest name, after the largest size.
    const ImGuiStyle& style = ImGui::GetStyle();
    float nameWidth = 0.f;

    for (const WhisperModel& model : whisperModels()) {
        nameWidth = std::max(nameWidth, ImGui::CalcTextSize(model.name).x);
    }

    const float sizeColumn = style.WindowPadding.x + ImGui::GetFrameHeight() + style.ItemInnerSpacing.x + nameWidth + 2.f * style.ItemSpacing.x;
    const float stateColumn = sizeColumn + ImGui::CalcTextSize("0000 MB").x + 2.f * style.ItemSpacing.x;

    for (const WhisperModel& model : whisperModels()) {
        ImGui::PushID(model.id);

        const bool downloaded = modelDownloaded(model);
        const bool downloading = _modelDownload.running() && _modelDownload.modelId() == model.id;

        ImGui::BeginDisabled(!downloaded);

        if (ImGui::RadioButton(model.name, _whisperModel == model.id)) {
            _whisperModel = model.id;
        }

        ImGui::EndDisabled();
        ImGui::SameLine(sizeColumn);
        ImGui::TextDisabled(tr("%d MB"), model.sizeMegabytes);
        ImGui::SameLine(stateColumn);

        if (downloaded) {
            ImGui::TextColored(ImVec4(0.4f, 0.85f, 0.5f, 1.f), tr(ICON_MD_CHECK " Ready"));
        }
        else if (downloading) {
            ImGui::ProgressBar(_modelDownload.progress(), ImVec2(-3.f * ImGui::GetFontSize(), 0.f));
            ImGui::SameLine();

            if (ImGui::SmallButton(ICON_MD_CLOSE)) {
                _modelDownload.cancel();
            }
        }
        else {
            ImGui::BeginDisabled(_modelDownload.running());

            if (ImGui::SmallButton(tr(ICON_MD_DOWNLOAD " Download"))) {
                _modelDownload.start(model);
            }

            ImGui::EndDisabled();
        }

        ImGui::SetItemTooltip("%s", model.description);
        ImGui::PopID();
    }

    if (!_modelDownload.running() && !_modelDownload.error().empty()) {
        ImGui::TextColored(ImVec4(0.9f, 0.35f, 0.3f, 1.f), ICON_MD_WARNING " %s", _modelDownload.error().c_str());
    }

    // Source, language and processor
    ImGui::SeparatorText(tr("Speech"));

    const Track* sourceTrack = _project.findTrack(_subtitleSourceTrack);

    if (!sourceTrack) {
        _subtitleSourceTrack = InvalidId;
    }

    ImGui::SetNextItemWidth(12.f * ImGui::GetFontSize());

    if (ImGui::BeginCombo(tr("Sound"), sourceTrack ? sourceTrack->name.c_str() : "Whole sequence")) {
        if (ImGui::Selectable(tr("Whole sequence"), !sourceTrack)) {
            _subtitleSourceTrack = InvalidId;
        }

        for (const auto* tracks : { &_project.sequence().soundTracks, &_project.sequence().audioTracks }) {
            for (const Track& track : *tracks) {
                if (ImGui::Selectable(track.name.c_str(), sourceTrack && sourceTrack->id == track.id)) {
                    _subtitleSourceTrack = track.id;
                }
            }
        }

        ImGui::EndCombo();
    }

    const char* languages[][2] = {
        { "auto", "Detected" }, { "fr", "Fran\xc3\xa7" "ais" }, { "en", "English" }, { "es", "Espa\xc3\xb1ol" },
        { "de", "Deutsch" }, { "it", "Italiano" }, { "pt", "Portugu\xc3\xaas" }, { "nl", "Nederlands" },
    };

    ImGui::SetNextItemWidth(12.f * ImGui::GetFontSize());

    if (ImGui::BeginCombo(tr("Language"), languages[_whisperLanguage][1])) {
        for (int i = 0; i < (int)IM_ARRAYSIZE(languages); i++) {
            if (ImGui::Selectable(languages[i][1], i == _whisperLanguage)) _whisperLanguage = i;
        }

        ImGui::EndCombo();
    }

    ImGui::Checkbox(tr("Use the GPU"), &_whisperGpu);
    ImGui::SetItemTooltip(tr("Off: on the processor only, much slower with the large models."));
    ImGui::EndDisabled();

    ImGui::Spacing();

    // Generate
    const WhisperModel* chosen = nullptr;

    for (const WhisperModel& model : whisperModels()) {
        if (model.id == _whisperModel && modelDownloaded(model)) chosen = &model;
    }

    // Test: as soon as the sound is decoded.
    bool testStart = false;

    if (_testTranscribe && !busy && chosen && !_subtitleGenerator.done() && _testQuitTicks == 0) {
        std::string error;
        testStart = !speechSamples(error).empty();
    }

    if (!busy) {
        ImGui::BeginDisabled(!chosen);

        if (ImGui::Button(tr(ICON_MD_AUTO_AWESOME " Generate"), ImVec2(10.f * ImGui::GetFontSize(), 0.f)) || testStart) {
            _generatorError.clear();
            std::vector<float> samples = speechSamples(_generatorError);

            if (!samples.empty()) {
                SubtitleGenerator::Settings settings;
                settings.model = modelFolder() / chosen->file;
                settings.language = languages[_whisperLanguage][0];
                settings.useGpu = _whisperGpu;
                _subtitleGenerator.start(std::move(samples), settings);
            }
        }

        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled(chosen ? tr("Onto a new subtitle track.") : tr("Download a model first."));
    }
    else {
        ImGui::ProgressBar(_subtitleGenerator.progress(), ImVec2(-6.f * ImGui::GetFontSize(), 0.f), tr("Listening..."));
        ImGui::SameLine();

        if (ImGui::Button(tr("Cancel"))) {
            _subtitleGenerator.cancel();
        }
    }

    if (!_generatorError.empty()) {
        ImGui::PushTextWrapPos(0.f);
        ImGui::TextColored(ImVec4(0.9f, 0.35f, 0.3f, 1.f), ICON_MD_WARNING " %s", _generatorError.c_str());
        ImGui::PopTextWrapPos();
    }

    ImGui::End();
}


void GUI::importSubtitles(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary);
    std::stringstream text;
    text << file.rdbuf();

    const std::vector<SubtitleCue> cues = parseSrt(text.str());

    if (cues.empty()) {
        setErrorMessage(tr("No subtitle could be read from ") + pathToUtf8(path.filename()) + ".");
        return;
    }

    // A track of their own, named after the file.
    Sequence& sequence = _project.sequence();
    const Id trackId = _project.newId();
    sequence.addTrack(TrackType::Subtitle, trackId, InvalidId);
    sequence.subtitleTracks.back().name = pathToUtf8(path.stem());

    for (const SubtitleCue& cue : cues) {
        _project.addTextClip(trackId, cue.start, cue.end - cue.start, cue.text);
    }

    commit("Import subtitles");
}


bool GUI::exportSubtitles(const std::filesystem::path& path)
{
    const std::vector<SubtitleCue> cues = subtitleCues(_project.sequence());

    if (cues.empty()) {
        return false;
    }

    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file << toSrt(cues);

    if (!file) {
        setErrorMessage(tr("Could not write ") + pathToUtf8(path) + ".");
        return false;
    }

    return true;
}


void SDLCALL GUI::onSubtitleFile(void* userdata, const char* const* filelist, int)
{
    if (!userdata || !filelist || !filelist[0]) {
        return;
    }

    GUI* gui = static_cast<GUI*>(userdata);
    std::filesystem::path filepath = std::filesystem::u8path(filelist[0]);

    if (gui->_subtitleDialogSaves && filepath.extension().empty()) {
        filepath += ".srt";
    }

    std::scoped_lock lock(gui->_dialogMutex);
    gui->_dialogSubtitlePath = filepath;
}


void GUI::testFileDrop(const std::filesystem::path& path, float x, float y)
{
    _testDrag.active = true;
    _testDrag.files = { path };
    _testDrag.x = x;
    _testDrag.y = y;
}


void GUI::fileDragPreview()
{
    const FileDrag& drag = fileDrag();

    if (!drag.active) {
        _dropProbes.clear();
        return;
    }

    // Their lengths, once all known.
    TimelineView::FileDrop drop;
    drop.active = true;
    drop.dropped = drag.dropped;
    bool known = !drag.files.empty();

    for (const std::filesystem::path& path : drag.files) {
        if (path.extension() == std::string(".") + Project::FileExtension) {
            continue;
        }

        // Imported already.
        const auto media = std::find_if(_project.media().begin(), _project.media().end(),
            [&](const MediaItem& item) { return item.path == path.lexically_normal(); });

        if (media != _project.media().end() && media->probed) {
            drop.durations.push_back(media->duration);
            continue;
        }

        std::shared_ptr<DropProbe>& probe = _dropProbes[path];

        if (!probe) {
            probe = std::make_shared<DropProbe>();

            _threadPool.submit([probe, path] {
                try {
                    probe->result = media::probe(path);
                    probe->ok = true;
                }
                catch (const std::exception&) {
                }

                probe->done.store(true);
            });
        }

        if (!probe->done.load()) {
            known = false;
        }
        else if (probe->ok) {
            const media::ProbeResult& result = probe->result;
            drop.durations.push_back((result.isImage || result.duration <= 0) ? DefaultImageDuration : result.duration);
        }
    }

    if (!known) {
        drop.durations.clear();
    }

    _timeline.setFileDrop(std::move(drop));
}


void GUI::fileDropped()
{
    if (!fileDrag().dropped) {
        return;
    }

    const std::vector<std::filesystem::path> files = fileDrag().files;
    const Id trackId = std::exchange(_dropTrack, InvalidId);
    endFileDrag();

    // Elsewhere than on a track: imported, or the project opened.
    if (trackId == InvalidId) {
        for (const std::filesystem::path& path : files) {
            eventFileDropped(path);
        }

        _dropProbes.clear();
        return;
    }

    // On a track: one after the other from where they were dropped.
    std::vector<Id> added;
    Time start = _dropStart;

    for (const std::filesystem::path& path : files) {
        if (path.extension() == std::string(".") + Project::FileExtension) {
            continue;
        }

        const Id mediaId = _project.importMedia(path);
        MediaItem* item = _project.findMedia(mediaId);

        if (!item) {
            continue;
        }

        // Its properties now, for the length of its clip: from the probe
        // of the drag when done.
        if (!item->probed) {
            const auto it = _dropProbes.find(path);
            const bool probed = it != _dropProbes.end() && it->second->done.load();

            try {
                const media::ProbeResult result = probed ? it->second->result : media::probe(path);

                if (!probed || it->second->ok) {
                    media::MediaManager::applyProbe(*item, result);
                }
            }
            catch (const std::exception&) {
                // Unreadable: imported only, the media panel tells why.
                continue;
            }

            if (!item->probed) {
                continue;
            }
        }

        const Id clipId = _project.addClip(mediaId, trackId, start);

        if (const Clip* clip = _project.findClip(clipId)) {
            added.push_back(clipId);
            start = clip->end();
        }
    }

    _dropProbes.clear();

    _selection.clear();

    for (Id clipId : added) {
        TimelineView::addWithLinks(_project, clipId, _selection);
    }

    _selectedMedia = InvalidId;
    commit(added.empty() ? "Import media" : "Add clips");
}


void GUI::recorderWindow()
{
    // Closed: the input too (the system shows it in use while open), unless
    // the screen recording uses it.
    if (!_showRecorder && !_screenRecording && !(_showScreenRecorder && _captureMicrophone)) {
        if (_recorder.isOpen()) {
            if (_recorder.recording()) {
                stopRecording();
            }

            _recorder.close();
            _recorderError.clear();
        }

        return;
    }

    if (!_showRecorder) {
        return;
    }

    if (!_inputDevicesListed) {
        _inputDevices = audio::Recorder::devices();
        _inputDevicesListed = true;
    }

    if (!_recorder.isOpen() && _recorderError.empty()) {
        _recorder.open(_inputDevice, _recorderError);
    }

    ImGui::SetNextWindowSize(ImVec2(28.f * ImGui::GetFontSize(), 0.f), ImGuiCond_Appearing);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

    if (!ImGui::Begin(tr(ICON_MD_MIC " Record Voice-Over###recorder"), &_showRecorder, ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }

    // The microphone records with the screen.
    if (_screenRecording && !_screenMicrophonePath.empty()) {
        ImGui::TextDisabled(tr(ICON_MD_SCREEN_SHARE " The microphone is recording with the screen."));
        ImGui::End();
        return;
    }

    const bool recording = _recorder.recording();

    // Input
    std::string current = tr("Default input");

    for (const audio::InputDevice& device : _inputDevices) {
        if (device.id == _inputDevice) current = device.name;
    }

    ImGui::BeginDisabled(recording);
    ImGui::SetNextItemWidth(-ImGui::GetFrameHeight() - ImGui::GetStyle().ItemSpacing.x);

    if (ImGui::BeginCombo("##input", (std::string(ICON_MD_MIC " ") + current).c_str())) {
        auto choose = [&](SDL_AudioDeviceID id, const std::string& name) {
            if (ImGui::Selectable(name.c_str(), id == _inputDevice) && (id != _inputDevice || !_recorder.isOpen())) {
                _inputDevice = id;
                _recorderError.clear();
                _recorder.open(_inputDevice, _recorderError);
            }
        };

        choose(0, tr("Default input"));

        for (const audio::InputDevice& device : _inputDevices) {
            choose(device.id, device.name);
        }

        ImGui::EndCombo();
    }

    ImGui::SameLine();

    if (ImGui::Button(ICON_MD_REFRESH, ImVec2(ImGui::GetFrameHeight(), 0.f))) {
        _inputDevicesListed = false;
    }

    ImGui::SetItemTooltip(tr("Look for inputs again"));
    ImGui::EndDisabled();

    if (!_recorderError.empty()) {
        ImGui::TextColored(ImVec4(0.9f, 0.35f, 0.3f, 1.f), ICON_MD_WARNING " %s", _recorderError.c_str());
    }

    // Level
    ImGui::Spacing();
    levelMeter("##input", _recorder.takePeak(), _inputMeter);
    ImGui::Spacing();

    ImGui::BeginDisabled(recording);
    ImGui::Checkbox(tr("Play the sequence while recording"), &_playWhileRecording);
    ImGui::EndDisabled();

    if (_playWhileRecording) {
        ImGui::TextDisabled(tr("Use headphones, or the speakers will be recorded too."));
    }

    ImGui::Spacing();

    // Record / Stop
    const float width = 8.f * ImGui::GetFontSize();

    if (!recording) {
        ImGui::BeginDisabled(!_recorder.isOpen());
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.75f, 0.18f, 0.16f, 1.f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.85f, 0.25f, 0.22f, 1.f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.65f, 0.14f, 0.12f, 1.f));

        if (ImGui::Button(tr(ICON_MD_FIBER_MANUAL_RECORD " Record"), ImVec2(width, 0.f))) {
            startRecording();
        }

        ImGui::PopStyleColor(3);
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled(tr("From the playhead, %s"), formatTimecode(_playhead, _project.sequence().frameRate).c_str());
    }
    else {
        if (ImGui::Button(tr(ICON_MD_STOP " Stop"), ImVec2(width, 0.f))) {
            stopRecording();
        }

        // Blinking dot and length.
        const Time length = mulDivFloor(_recorder.recordedFrames(), TicksPerSecond, audio::Recorder::SampleRate);
        const bool on = std::fmod(ImGui::GetTime(), 1.0) < 0.6;

        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(ImVec4(0.95f, 0.3f, 0.25f, on ? 1.f : 0.25f), ICON_MD_FIBER_MANUAL_RECORD);
        ImGui::SameLine();
        ImGui::Text(tr("Recording  %s"), formatTimecode(length, _project.sequence().frameRate).c_str());
    }

    ImGui::Spacing();
    ImGui::PushTextWrapPos(0.f);
    ImGui::TextDisabled(tr("Placed on the first free audio track. Saved in %s."),
        _projectPath.empty() ? tr("Documents/QuickEdit/Recordings") : tr("the Recordings folder next to the project"));
    ImGui::PopTextWrapPos();

    ImGui::End();
}


void GUI::levelMeter(const char* id, float peak, LevelMeter& meter)
{
    // Peak level, falling back slowly; the highest peak held a moment.
    constexpr float MinDb = -60.f;
    const double now = ImGui::GetTime();
    const float peakDb = (peak > 0.f) ? 20.f * std::log10(peak) : -100.f;

    meter.levelDb = std::max(peakDb, meter.levelDb - 30.f * ImGui::GetIO().DeltaTime);

    if (peakDb >= meter.peakDb || now - meter.peakTime > 1.5) {
        meter.peakDb = std::max(peakDb, meter.levelDb);
        meter.peakTime = now;
    }

    if (peak >= 0.999f) {
        meter.clipTime = now;
    }

    const ImGuiStyle& style = ImGui::GetStyle();
    const bool clipping = now - meter.clipTime < 2.0;
    const char* clipLabel = "CLIP";
    const float labelWidth = ImGui::CalcTextSize("-00.0 dB").x;
    const float clipWidth = ImGui::CalcTextSize(clipLabel).x + 2.f * style.FramePadding.x;

    const ImVec2 min = ImGui::GetCursorScreenPos();
    const float width = std::max(1.f, ImGui::GetContentRegionAvail().x - labelWidth - clipWidth - 2.f * style.ItemSpacing.x);
    const float height = ImGui::GetFrameHeight();
    const ImVec2 max(min.x + width, min.y + height);

    auto xOf = [&](float db) { return min.x + width * std::clamp((db - MinDb) / -MinDb, 0.f, 1.f); };

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    drawList->AddRectFilled(min, max, ImGui::GetColorU32(ImGuiCol_FrameBg), 3.f);

    // Green, yellow from -18 dB, red from -6 dB.
    const struct { float from, to; ImU32 color; } ranges[] = {
        { MinDb, -18.f, IM_COL32(70, 190, 100, 255) },
        { -18.f, -6.f, IM_COL32(230, 200, 60, 255) },
        { -6.f, 0.f, IM_COL32(230, 70, 60, 255) },
    };

    for (const auto& range : ranges) {
        const float to = std::min(range.to, meter.levelDb);

        if (to > range.from) {
            drawList->AddRectFilled(ImVec2(xOf(range.from), min.y + 3.f), ImVec2(xOf(to), max.y - 3.f), range.color, 2.f);
        }
    }

    // Graduations
    for (float db : { -48.f, -36.f, -24.f, -18.f, -12.f, -6.f, -3.f }) {
        drawList->AddLine(ImVec2(xOf(db), max.y - 5.f), ImVec2(xOf(db), max.y - 1.f), IM_COL32(255, 255, 255, 90));
    }

    if (meter.peakDb > MinDb) {
        drawList->AddLine(ImVec2(xOf(meter.peakDb), min.y + 2.f), ImVec2(xOf(meter.peakDb), max.y - 2.f), IM_COL32(255, 255, 255, 220), 2.f);
    }

    ImGui::PushID(id);
    ImGui::Dummy(ImVec2(width, height));
    ImGui::SetItemTooltip(tr("Aim for peaks between -12 and -6 dB."));

    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();

    if (meter.peakDb > MinDb) {
        ImGui::Text(tr("%5.1f dB"), meter.peakDb);
    }
    else {
        ImGui::TextDisabled(tr(" -inf dB"));
    }

    // Clipping lamp
    ImGui::SameLine();
    const ImVec2 lampMin = ImGui::GetCursorScreenPos();
    const ImVec2 lampMax(lampMin.x + clipWidth, lampMin.y + height);

    drawList->AddRectFilled(lampMin, lampMax, clipping ? IM_COL32(220, 50, 40, 255) : ImGui::GetColorU32(ImGuiCol_FrameBg), 3.f);
    drawList->AddText(ImVec2(lampMin.x + style.FramePadding.x, lampMin.y + style.FramePadding.y),
        clipping ? IM_COL32(255, 255, 255, 255) : ImGui::GetColorU32(ImGuiCol_TextDisabled), clipLabel);
    ImGui::Dummy(ImVec2(clipWidth, height));
    ImGui::SetItemTooltip(tr("The input saturates: lower its gain."));
    ImGui::PopID();
}


void GUI::updateLiveWaveform()
{
    if (!_liveWaveform || !_recorder.recording()) {
        return;
    }

    std::vector<std::vector<media::Waveform::Peak>>& levels = _liveWaveform->levels;

    if (levels.empty()) {
        levels.emplace_back();
    }

    _recorder.takePeaks(levels[0]);

    // Each level pairs the peaks of the previous one, down to a few peaks.
    for (size_t level = 1; levels[level - 1].size() >= 2 && level < 16; level++) {
        if (levels.size() <= level) {
            levels.emplace_back();
        }

        const std::vector<media::Waveform::Peak>& finer = levels[level - 1];
        std::vector<media::Waveform::Peak>& coarser = levels[level];

        while (2 * coarser.size() + 1 < finer.size()) {
            const media::Waveform::Peak& a = finer[2 * coarser.size()];
            const media::Waveform::Peak& b = finer[2 * coarser.size() + 1];
            coarser.push_back({ std::min(a.min, b.min), std::max(a.max, b.max) });
        }
    }
}


std::filesystem::path GUI::recordingPath(const char* prefix, const char* extension) const
{
    std::filesystem::path folder;

    if (!_projectPath.empty()) {
        folder = _projectPath.parent_path() / "Recordings";
    }
    else if (const char* documents = SDL_GetUserFolder(SDL_FOLDER_DOCUMENTS)) {
        folder = std::filesystem::u8path(documents) / "QuickEdit" / "Recordings";
    }
    else {
        folder = std::filesystem::temp_directory_path() / "QuickEdit Recordings";
    }

    const std::time_t now = std::time(nullptr);
    std::tm local = {};
#ifdef _WIN32
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif

    char date[32];
    std::strftime(date, sizeof(date), " %Y-%m-%d %H-%M-%S", &local);

    return folder / std::filesystem::u8path(std::string(prefix) + date + extension);
}


void GUI::startRecording()
{
    const std::filesystem::path path = recordingPath("Voice-over", ".wav");
    _recorderError.clear();

    if (!_recorder.start(path, _recorderError)) {
        return;
    }

    _recordPath = path;

    // From the playhead, with the sequence playing if asked.
    if (!_playing) {
        _playhead = snapToFrame(_playhead, _project.sequence().frameRate);
    }

    _recordStart = _playhead;

    // Its track: the first audio track free from the playhead on, or a new
    // one, recorded in the history with the clip.
    const Sequence& sequence = _project.sequence();
    _recordTrack = InvalidId;
    _recordTrackAdded = false;

    for (const Track& track : sequence.audioTracks) {
        const bool free = std::none_of(track.clips.begin(), track.clips.end(),
            [&](const Clip& clip) { return clip.end() > _recordStart; });

        if (free && !track.locked) {
            _recordTrack = track.id;
            break;
        }
    }

    if (_recordTrack == InvalidId) {
        _recordTrack = _project.newId();
        _recordTrackAdded = true;
        _project.sequence().addTrack(TrackType::Audio, _recordTrack, InvalidId);
    }

    _liveWaveform = std::make_shared<media::Waveform>();
    _liveWaveform->sampleRate = audio::Recorder::SampleRate;

    if (_playWhileRecording && !_playing) {
        _playing = true;
        _audio.play(_playhead);
    }
    else if (!_playWhileRecording) {
        stopPlayback();
    }
}


void GUI::stopRecording()
{
    const int64_t frames = _recorder.stop();
    stopPlayback();
    _liveWaveform.reset();

    Sequence& sequence = _project.sequence();

    if (frames <= 0) {
        std::error_code ec;
        std::filesystem::remove(_recordPath, ec);

        // Nothing to place on the track added for it.
        if (_recordTrackAdded) {
            std::erase_if(sequence.audioTracks, [&](const Track& track) { return track.id == _recordTrack && track.clips.empty(); });
        }

        return;
    }

    const Time duration = mulDivFloor(frames, TicksPerSecond, audio::Recorder::SampleRate);
    const Time end = _recordStart + duration;

    // The track chosen at the start, unless edits since took it (undo of
    // the track added, a clip moved there): then the first free one, or a
    // new one.
    auto freeOver = [&](const Track& track) {
        return !track.locked && std::none_of(track.clips.begin(), track.clips.end(),
            [&](const Clip& clip) { return clip.start < end && clip.end() > _recordStart; });
    };

    const Track* chosen = _project.findTrack(_recordTrack);
    Id trackId = (chosen && chosen->type == TrackType::Audio && freeOver(*chosen)) ? _recordTrack : InvalidId;

    for (const Track& track : sequence.audioTracks) {
        if (trackId != InvalidId) {
            break;
        }

        if (freeOver(track)) {
            trackId = track.id;
        }
    }

    if (trackId == InvalidId) {
        trackId = _project.newId();
        sequence.addTrack(TrackType::Audio, trackId, InvalidId);
    }

    // Known already: no need to wait for the probe.
    const Id mediaId = _project.importMedia(_recordPath);

    if (MediaItem* item = _project.findMedia(mediaId)) {
        item->probed = true;
        item->type = MediaType::Audio;
        item->duration = duration;
        item->hasAudio = true;
        item->sampleRate = audio::Recorder::SampleRate;
        item->channels = 1;
    }

    const Id clipId = _project.addClip(mediaId, trackId, _recordStart);

    _selection.clear();

    if (clipId != InvalidId) {
        _selection.push_back(clipId);
    }

    _selectedMedia = mediaId;
    _playhead = end;
    commit("Record voice-over");
}


void GUI::screenRecorderWindow()
{
    // The windows of QuickEdit stay out of the capture, the ones opened
    // during it too.
    if (_screenRecording && _hideFromCapture) {
        capture::excludeFromCapture(true);
    }

    // The capture ended by itself (window closed): what was recorded is kept.
    if (_screenRecording && !_screenRecorder.recording()) {
        stopScreenRecording();
    }

    if (!_showScreenRecorder) {
        if (_screenRecording) {
            stopScreenRecording();
        }

        _sourcePreview.stop();
        return;
    }

    if (!_captureSourcesListed) {
        _captureSources = capture::listSources();
        _captureSourcesListed = true;

        // The source chosen before, while it exists; the main screen first.
        const bool known = std::any_of(_captureSources.begin(), _captureSources.end(),
            [&](const capture::CaptureSource& source) { return source.sameDevice(_captureSource); });

        if (!known && !_captureSources.empty()) {
            _captureSource = _captureSources.front();

            for (const capture::CaptureSource& source : _captureSources) {
                if (source.kind == capture::CaptureSource::Kind::Monitor && source.name.find(", main)") != std::string::npos) {
                    _captureSource = source;
                }
            }
        }
    }

    // The microphone, for its meter.
    if (_captureMicrophone && !_recorder.isOpen() && _recorderError.empty()) {
        _recorder.open(_inputDevice, _recorderError);
    }

    ImGui::SetNextWindowSize(ImVec2(30.f * ImGui::GetFontSize(), 0.f), ImGuiCond_Appearing);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

    if (!ImGui::Begin(tr(ICON_MD_SCREEN_SHARE " Record Screen###screenRecorder"), &_showScreenRecorder, ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }

    const bool recording = _screenRecording;
    const bool isMonitor = _captureSource.kind == capture::CaptureSource::Kind::Monitor;
    const bool isCamera = _captureSource.kind == capture::CaptureSource::Kind::Camera;

    auto sourceIcon = [](const capture::CaptureSource& source) {
        return source.kind == capture::CaptureSource::Kind::Monitor ? ICON_MD_MONITOR " "
            : source.kind == capture::CaptureSource::Kind::Camera ? ICON_MD_VIDEOCAM " " : ICON_MD_WEB_ASSET " ";
    };

    // Source
    ImGui::BeginDisabled(recording);
    ImGui::SetNextItemWidth(-ImGui::GetFrameHeight() - ImGui::GetStyle().ItemSpacing.x);

    const std::string preview = _captureSource.handle
        ? std::string(sourceIcon(_captureSource)) + _captureSource.name
        : tr("Choose a screen, a camera or a window...");

    if (ImGui::BeginCombo("##source", preview.c_str(), ImGuiComboFlags_HeightLarge)) {
        const char* titles[] = { tr("Screens"), tr("Windows"), tr("Cameras") };
        int group = -1;

        for (const capture::CaptureSource& source : _captureSources) {
            if ((int)source.kind != group) {
                group = (int)source.kind;
                ImGui::SeparatorText(titles[group]);
            }

            ImGui::PushID((const void*)(uintptr_t)source.handle);
            ImGui::PushID(source.device.c_str());
            const std::string label = std::string(sourceIcon(source)) + source.name;

            if (ImGui::Selectable(label.c_str(), source.sameDevice(_captureSource)) && !source.sameDevice(_captureSource)) {
                _captureSource = source;
                _screenError.clear();
            }

            ImGui::PopID();
            ImGui::PopID();
        }

        ImGui::EndCombo();
    }

    ImGui::SameLine();

    if (ImGui::Button(ICON_MD_REFRESH "##sources", ImVec2(ImGui::GetFrameHeight(), 0.f))) {
        _captureSourcesListed = false;
    }

    ImGui::SetItemTooltip(tr("Look for screens and windows again"));

    if (isCamera) {
        cameraFormatOptions();
    }

    sourcePreview();

    // Options. A camera has its own frame rate, and no cursor.
    if (!isCamera) {
        // The frame rate of the sequence by default (0), so that the
        // pictures fall on its frames.
        const Rational& sequenceRate = _project.sequence().frameRate;
        char sequenceLabel[48];
        std::snprintf(sequenceLabel, sizeof(sequenceLabel), tr("Sequence (%g)"), (double)sequenceRate.num / (double)sequenceRate.den);

        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(tr("Frame rate"));
        ImGui::SameLine();
        ImGui::RadioButton(sequenceLabel, &_captureFrameRate, 0);
        ImGui::SameLine();
        ImGui::RadioButton("30", &_captureFrameRate, 30);
        ImGui::SameLine();
        ImGui::RadioButton("60", &_captureFrameRate, 60);
        ImGui::Checkbox(tr("Mouse cursor"), &_captureCursor);
        ImGui::SameLine();
        ImGui::Checkbox(tr("Hide QuickEdit from the recording"), &_hideFromCapture);
    }
    ImGui::EndDisabled();

    // Sound of the computer and microphone, with their levels.
    ImGui::Spacing();
    ImGui::BeginDisabled(recording);
    ImGui::Checkbox(tr("Computer sound"), &_captureSound);
    ImGui::EndDisabled();

    if (_captureSound) {
        levelMeter("##system", _screenRecorder.takeAudioPeak(), _systemMeter);
    }

    std::string microphone = tr("Default input");

    for (const audio::InputDevice& device : _inputDevices) {
        if (device.id == _inputDevice) microphone = device.name;
    }

    ImGui::BeginDisabled(recording);
    ImGui::Checkbox((tr("Microphone: ") + microphone + "##microphone").c_str(), &_captureMicrophone);
    ImGui::SetItemTooltip(tr("The input chosen in the Record Voice-Over window. Recorded on an audio track of its own."));
    ImGui::EndDisabled();

    if (_captureMicrophone) {
        levelMeter("##microphone", _recorder.takePeak(), _microphoneMeter);

        if (!_recorderError.empty()) {
            ImGui::TextColored(ImVec4(0.9f, 0.35f, 0.3f, 1.f), ICON_MD_WARNING " %s", _recorderError.c_str());
        }
    }

    ImGui::Spacing();

    // Record / Stop
    const float width = 8.f * ImGui::GetFontSize();

    if (!recording) {
        ImGui::BeginDisabled(_captureSource.handle == 0);
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.75f, 0.18f, 0.16f, 1.f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.85f, 0.25f, 0.22f, 1.f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.65f, 0.14f, 0.12f, 1.f));

        if (ImGui::Button(tr(ICON_MD_FIBER_MANUAL_RECORD " Record"), ImVec2(width, 0.f))) {
            startScreenRecording();
        }

        ImGui::PopStyleColor(3);
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled(tr("At the playhead, %s"), formatTimecode(_playhead, _project.sequence().frameRate).c_str());
    }
    else {
        if (ImGui::Button(tr(ICON_MD_STOP " Stop"), ImVec2(width, 0.f))) {
            stopScreenRecording();
        }

        const bool on = std::fmod(ImGui::GetTime(), 1.0) < 0.6;
        const Time length = secondsToTime(_screenRecorder.seconds());

        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(ImVec4(0.95f, 0.3f, 0.25f, on ? 1.f : 0.25f), ICON_MD_FIBER_MANUAL_RECORD);
        ImGui::SameLine();
        ImGui::Text(tr("Recording  %s"), formatTimecode(length, _project.sequence().frameRate).c_str());
        ImGui::SameLine();
        ImGui::TextDisabled("(%s)", _screenRecorder.encoder().c_str());
    }

    if (!_screenError.empty()) {
        ImGui::PushTextWrapPos(0.f);
        ImGui::TextColored(ImVec4(0.9f, 0.35f, 0.3f, 1.f), ICON_MD_WARNING " %s", _screenError.c_str());
        ImGui::PopTextWrapPos();
    }

    ImGui::Spacing();
    ImGui::PushTextWrapPos(0.f);
    ImGui::TextDisabled(tr("Placed on the first free video track (the microphone on an audio track). Saved in %s."),
        _projectPath.empty() ? tr("Documents/QuickEdit/Recordings") : tr("the Recordings folder next to the project"));
    ImGui::PopTextWrapPos();

    ImGui::End();
}


void GUI::cameraFormatOptions()
{
    // The formats of the camera, read once per camera.
    if (_cameraFormatsDevice != _captureSource.device) {
        _cameraFormatsDevice = _captureSource.device;
        _cameraFormats = capture::listCameraFormats(_captureSource);
    }

    capture::CameraFormat& chosen = _captureSource.format;
    const float labelWidth = ImGui::CalcTextSize(tr("Frame rate")).x + ImGui::GetStyle().ItemSpacing.x;
    const float half = 0.5f * (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x);

    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(tr("Format"));
    ImGui::SameLine(labelWidth);
    ImGui::SetNextItemWidth(half - labelWidth);

    if (ImGui::BeginCombo("##cameraFormat", chosen.label().c_str(), ImGuiComboFlags_HeightLarge)) {
        if (ImGui::Selectable(tr("Automatic"), chosen.automatic())) {
            chosen = capture::CameraFormat();
            _captureSource.frameRate = 0.0;
        }

        ImGui::SetItemTooltip(tr("The largest size the camera gives in MJPEG, up to 1080p"));

        for (const capture::CameraFormat& format : _cameraFormats) {
            ImGui::PushID(&format);

            if (ImGui::Selectable(format.label().c_str(), format.sameMode(chosen))) {
                chosen = format;

                // The rate chosen before when offered, else the highest.
                const auto& rates = format.frameRates;

                if (std::find(rates.begin(), rates.end(), _captureSource.frameRate) == rates.end()) {
                    _captureSource.frameRate = rates.empty() ? 0.0 : rates.front();
                }
            }

            ImGui::PopID();
        }

        ImGui::EndCombo();
    }

    if (_cameraFormats.empty()) {
        ImGui::SetItemTooltip(tr("The formats of this camera are not known: it gives its default one"));
    }

    // Frame rate, of the chosen format.
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(tr("Frame rate"));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-FLT_MIN);

    char rateLabel[32] = "";
    std::snprintf(rateLabel, sizeof(rateLabel), "%s", tr("Automatic"));

    if (_captureSource.frameRate > 0.0) {
        std::snprintf(rateLabel, sizeof(rateLabel), "%g fps", _captureSource.frameRate);
    }

    ImGui::BeginDisabled(chosen.automatic() || chosen.frameRates.empty());

    if (ImGui::BeginCombo("##cameraRate", rateLabel)) {
        for (double rate : chosen.frameRates) {
            char label[32];
            std::snprintf(label, sizeof(label), "%g fps", rate);

            if (ImGui::Selectable(label, rate == _captureSource.frameRate)) {
                _captureSource.frameRate = rate;
            }
        }

        ImGui::EndCombo();
    }

    ImGui::EndDisabled();
}


void GUI::sourcePreview()
{
    constexpr int Width = capture::SourcePreview::Width;
    constexpr int Height = capture::SourcePreview::Height;

    // Live while choosing; the capture has the source while recording.
    if (_screenRecording) {
        _sourcePreview.stop();
    }
    else {
        _sourcePreview.show(_captureSource);
    }

    if (!_previewTexture) {
        _previewTexture = IM_NEW(ImTextureData)();
        _previewTexture->Create(ImTextureFormat_RGBA32, Width, Height);
        ImGui::RegisterUserTexture(_previewTexture);
    }

    const bool fresh = _screenRecording ? _screenRecorder.takePreview(_previewPixels) : _sourcePreview.take(_previewPixels);

    if (fresh) {
        std::memcpy(_previewTexture->GetPixels(), _previewPixels.data(), std::min(_previewPixels.size(), (size_t)Width * Height * 4));

        if (_previewTexture->Status == ImTextureStatus_OK) {
            const ImTextureRect all = { 0, 0, (unsigned short)Width, (unsigned short)Height };
            _previewTexture->UpdateRect = all;
            _previewTexture->Updates.resize(0);
            _previewTexture->Updates.push_back(all);
            _previewTexture->SetStatus(ImTextureStatus_WantUpdates);
        }
    }

    const float width = ImGui::GetContentRegionAvail().x;
    const ImVec2 size(width, width * (float)Height / (float)Width);
    const ImVec2 min = ImGui::GetCursorScreenPos();

    ImGui::Image(_previewTexture->GetTexRef(), size);

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const ImVec2 max(min.x + size.x, min.y + size.y);
    drawList->AddRect(min, max, ImGui::GetColorU32(ImGuiCol_Border));

    // Over the picture: what happens.
    const char* message = nullptr;
    const std::string error = _sourcePreview.error();

    if (!_screenRecording && !error.empty()) {
        message = error.c_str();
    }

    // Recording: a red frame and a mark in the corner.
    if (_screenRecording) {
        const bool on = std::fmod(ImGui::GetTime(), 1.0) < 0.6;
        drawList->AddRect(min, max, IM_COL32(230, 60, 50, 255), 0.f, 0, 2.f);
        drawList->AddRectFilled(ImVec2(min.x + 6.f, min.y + 6.f), ImVec2(min.x + 6.f + ImGui::CalcTextSize(tr(ICON_MD_FIBER_MANUAL_RECORD " REC")).x + 12.f, min.y + 10.f + ImGui::GetFontSize()), IM_COL32(0, 0, 0, 170), 4.f);
        drawList->AddText(ImVec2(min.x + 12.f, min.y + 8.f), on ? IM_COL32(255, 90, 80, 255) : IM_COL32(255, 90, 80, 90), tr(ICON_MD_FIBER_MANUAL_RECORD " REC"));
    }

    if (message) {
        const ImVec2 textSize = ImGui::CalcTextSize(message);
        const ImVec2 at(min.x + 0.5f * (size.x - textSize.x), min.y + 0.5f * (size.y - textSize.y));
        drawList->AddRectFilled(ImVec2(at.x - 8.f, at.y - 4.f), ImVec2(at.x + textSize.x + 8.f, at.y + textSize.y + 4.f), IM_COL32(0, 0, 0, 170), 4.f);
        drawList->AddText(at, _screenRecording ? IM_COL32(255, 110, 100, 255) : IM_COL32(255, 255, 255, 220), message);
    }
}


void GUI::startScreenRecording()
{
    stopPlayback();
    _screenError.clear();

    capture::ScreenRecordSettings settings;
    settings.source = _captureSource;
    if (_captureFrameRate == 0) {
        settings.frameRateNum = (int)_project.sequence().frameRate.num;
        settings.frameRateDen = (int)_project.sequence().frameRate.den;
    }
    else {
        settings.frameRateNum = _captureFrameRate;
        settings.frameRateDen = 1;
    }
    settings.cursor = _captureCursor;
    settings.systemAudio = _captureSound;

    // With the default test source: the main screen.
    if (!settings.source.handle) {
        const std::vector<capture::CaptureSource> sources = capture::listSources();

        if (!sources.empty()) {
            settings.source = _captureSource = sources.front();
        }
    }

    const bool camera = settings.source.kind == capture::CaptureSource::Kind::Camera;

    if (_hideFromCapture && !camera) {
        capture::excludeFromCapture(true);
    }

    // The camera opens once: its preview first lets it go.
    _sourcePreview.stop(camera);

    const std::filesystem::path path = recordingPath(camera ? "Camera" : "Screen", ".mkv");

    if (!_screenRecorder.start(path, settings, _screenError)) {
        capture::excludeFromCapture(false);
        return;
    }

    _screenPath = path;
    _screenStart = snapToFrame(_playhead, _project.sequence().frameRate);
    _screenRecording = true;
    _screenMicrophonePath.clear();

    // Its tracks: free from the playhead on, or new ones, recorded in the
    // history with the clips.
    Sequence& sequence = _project.sequence();
    const Time start = _screenStart;

    auto freeFrom = [&](const Track& track) {
        return !track.locked && std::none_of(track.clips.begin(), track.clips.end(),
            [&](const Clip& clip) { return clip.end() > start; });
    };

    _screenTrack = InvalidId;
    _screenTrackAdded = false;

    for (size_t i = 0; i < sequence.videoTracks.size() && _screenTrack == InvalidId; i++) {
        if (freeFrom(sequence.videoTracks[i]) && (i >= sequence.soundTracks.size() || freeFrom(sequence.soundTracks[i]))) {
            _screenTrack = sequence.videoTracks[i].id;
        }
    }

    if (_screenTrack == InvalidId) {
        _screenTrack = _project.newId();
        _screenTrackAdded = true;
        sequence.addTrack(TrackType::Video, _screenTrack, _project.newId());
    }

    // The microphone beside it, in a file of its own.
    if (_captureMicrophone) {
        if (!_recorder.isOpen()) {
            _recorder.open(_inputDevice, _recorderError);
        }

        const std::filesystem::path microphonePath = recordingPath("Screen voice", ".wav");

        if (_recorder.isOpen() && _recorder.start(microphonePath, _recorderError)) {
            _screenMicrophonePath = microphonePath;

            _liveWaveform = std::make_shared<media::Waveform>();
            _liveWaveform->sampleRate = audio::Recorder::SampleRate;

            _screenMicrophoneTrack = InvalidId;
            _screenMicrophoneTrackAdded = false;

            for (const Track& track : sequence.audioTracks) {
                if (freeFrom(track)) {
                    _screenMicrophoneTrack = track.id;
                    break;
                }
            }

            if (_screenMicrophoneTrack == InvalidId) {
                _screenMicrophoneTrack = _project.newId();
                _screenMicrophoneTrackAdded = true;
                sequence.addTrack(TrackType::Audio, _screenMicrophoneTrack, InvalidId);
            }
        }
    }
}


void GUI::stopScreenRecording()
{
    if (!_screenRecording) {
        return;
    }

    _screenRecording = false;

    const double seconds = _screenRecorder.seconds();
    const bool ok = _screenRecorder.stop();
    const int64_t microphoneFrames = _screenMicrophonePath.empty() ? 0 : _recorder.stop();

    capture::excludeFromCapture(false);

    if (!ok) {
        _screenError = _screenRecorder.error();
    }

    std::error_code ec;
    Sequence& sequence = _project.sequence();
    _liveWaveform.reset();

    // Tracks added for nothing go.
    auto dropAdded = [&](Id trackId, bool added) {
        if (!added) {
            return;
        }

        for (size_t i = 0; i < sequence.videoTracks.size(); i++) {
            if (sequence.videoTracks[i].id == trackId && sequence.videoTracks[i].clips.empty() &&
                (i >= sequence.soundTracks.size() || sequence.soundTracks[i].clips.empty())) {
                sequence.videoTracks.erase(sequence.videoTracks.begin() + (std::ptrdiff_t)i);

                if (i < sequence.soundTracks.size()) {
                    sequence.soundTracks.erase(sequence.soundTracks.begin() + (std::ptrdiff_t)i);
                }

                return;
            }
        }

        std::erase_if(sequence.audioTracks, [&](const Track& track) { return track.id == trackId && track.clips.empty(); });
    };

    if (seconds <= 0.0) {
        std::filesystem::remove(_screenPath, ec);

        if (!_screenMicrophonePath.empty()) {
            std::filesystem::remove(_screenMicrophonePath, ec);
        }

        dropAdded(_screenTrack, _screenTrackAdded);
        dropAdded(_screenMicrophoneTrack, _screenMicrophoneTrackAdded);
        return;
    }

    const Time start = _screenStart;

    // The microphone in the file of the recording, as its second audio
    // stream: the clip plays both, and can show each one.
    bool microphoneInFile = false;

    if (microphoneFrames > 0) {
        std::string error;

        if (capture::addAudioStream(_screenPath, _screenMicrophonePath, "Microphone", error)) {
            std::filesystem::remove(_screenMicrophonePath, ec);
            microphoneInFile = true;
        }
        else {
            SDL_Log("Screen recording: the microphone stays apart: %s", error.c_str());
        }
    }

    // The picture: the video track chosen at the start while it is still
    // free, else the first free one, or a new one.
    auto freeFrom = [&](const Track& track) {
        return !track.locked && std::none_of(track.clips.begin(), track.clips.end(),
            [&](const Clip& clip) { return clip.end() > start; });
    };

    Id videoTrack = InvalidId;

    for (size_t i = 0; i < sequence.videoTracks.size(); i++) {
        const bool free = freeFrom(sequence.videoTracks[i]) && (i >= sequence.soundTracks.size() || freeFrom(sequence.soundTracks[i]));

        if (free && (videoTrack == InvalidId || sequence.videoTracks[i].id == _screenTrack)) {
            videoTrack = sequence.videoTracks[i].id;
        }
    }

    if (videoTrack == InvalidId) {
        videoTrack = _project.newId();
        sequence.addTrack(TrackType::Video, videoTrack, _project.newId());
    }

    _selection.clear();

    // Probed now: its length places the clip.
    const Id mediaId = _project.importMedia(_screenPath);

    if (MediaItem* item = _project.findMedia(mediaId)) {
        try {
            media::MediaManager::applyProbe(*item, media::probe(_screenPath));
        }
        catch (const std::exception& e) {
            SDL_Log("Screen recording: %s", e.what());
        }
    }

    const Id clipId = _project.addClip(mediaId, videoTrack, start);

    if (clipId != InvalidId) {
        TimelineView::addWithLinks(_project, clipId, _selection);

        // A capture smaller than the sequence (a window, a camera) is shown
        // at its resolution rather than enlarged.
        const MediaItem* item = _project.findMedia(mediaId);

        if (item && item->width > 0 && item->width <= sequence.width && item->height <= sequence.height) {
            _project.findClip(clipId)->fit = FitMode::Native;
        }
    }

    Time end = start + secondsToTime(seconds);

    if (const Clip* clip = _project.findClip(clipId)) {
        end = clip->end();
    }

    // The microphone apart (it could not join the file): the first audio
    // track free over it, or a new one.
    if (microphoneFrames > 0 && !microphoneInFile) {
        const Time microphoneEnd = start + mulDivFloor(microphoneFrames, TicksPerSecond, audio::Recorder::SampleRate);
        Id audioTrack = InvalidId;

        for (const Track& track : sequence.audioTracks) {
            const bool free = !track.locked && std::none_of(track.clips.begin(), track.clips.end(),
                [&](const Clip& clip) { return clip.start < microphoneEnd && clip.end() > start; });

            if (free && (audioTrack == InvalidId || track.id == _screenMicrophoneTrack)) {
                audioTrack = track.id;
            }
        }

        if (audioTrack == InvalidId) {
            audioTrack = _project.newId();
            sequence.addTrack(TrackType::Audio, audioTrack, InvalidId);
        }

        const Id voiceId = _project.importMedia(_screenMicrophonePath);

        if (MediaItem* item = _project.findMedia(voiceId)) {
            item->probed = true;
            item->type = MediaType::Audio;
            item->duration = microphoneEnd - start;
            item->hasAudio = true;
            item->sampleRate = audio::Recorder::SampleRate;
            item->channels = 1;
        }

        if (const Id voiceClip = _project.addClip(voiceId, audioTrack, start); voiceClip != InvalidId) {
            _selection.push_back(voiceClip);
        }
    }
    else {
        if (!_screenMicrophonePath.empty()) {
            std::filesystem::remove(_screenMicrophonePath, ec);
        }

        dropAdded(_screenMicrophoneTrack, _screenMicrophoneTrackAdded);
    }

    // The new video track, when another one took the picture.
    if (videoTrack != _screenTrack) {
        dropAdded(_screenTrack, _screenTrackAdded);
    }

    _selectedMedia = mediaId;
    _playhead = end;
    commit("Record screen");
}


void GUI::closeGap()
{
    const TimelineView::Gap gap = _timeline.gap();
    _timeline.clearGap();

    if (_project.closeGap(gap.trackId, gap.start, gap.end)) {
        commit("Close gap");
    }
}


void GUI::toggleTransition()
{
    std::vector<Clip*> clips;

    for (Id id : _selection) {
        const Track* track = _project.findTrackOfClip(id);

        if (track && track->type == TrackType::Video) {
            clips.push_back(_project.findClip(id));
        }
    }

    // Nothing selected: the top clip at the playhead.
    const Sequence& sequence = _project.sequence();

    for (int i = (int)sequence.videoTracks.size() - 1; i >= 0 && clips.empty(); i--) {
        const int index = sequence.videoTracks[i].clipAt(_playhead);

        if (index >= 0) {
            clips.push_back(_project.findClip(sequence.videoTracks[i].clips[index].id));
        }
    }

    if (clips.empty()) {
        return;
    }

    const bool remove = std::all_of(clips.begin(), clips.end(), [](const Clip* c) { return c->transitionIn.type != TransitionType::None; });

    for (Clip* clip : clips) {
        clip->transitionIn.type = remove ? TransitionType::None : TransitionType::Dissolve;
    }

    commit(remove ? "Remove transition" : "Add transition");
}


void GUI::freezeFrame()
{
    // The selected video clip at the playhead, or the top one.
    const Sequence& sequence = _project.sequence();
    Id clipId = InvalidId;

    for (Id id : _selection) {
        const Clip* clip = _project.findClip(id);
        const Track* track = _project.findTrackOfClip(id);

        if (clip && track && track->type == TrackType::Video && clip->kind == ClipKind::Media &&
            _playhead >= clip->start && _playhead < clip->end()) {
            clipId = id;
        }
    }

    for (int i = (int)sequence.videoTracks.size() - 1; i >= 0 && clipId == InvalidId; i--) {
        const Track& track = sequence.videoTracks[i];
        const int index = track.clipAt(_playhead);

        if (index >= 0 && track.clips[index].kind == ClipKind::Media) {
            clipId = track.clips[index].id;
        }
    }

    if (clipId == InvalidId) {
        return;
    }

    stopPlayback();

    const Id still = _project.insertFreezeFrame(clipId, snapToFrame(_playhead, sequence.frameRate), 3 * TicksPerSecond);

    if (still != InvalidId) {
        _selection = { still };
        commit("Freeze frame");
    }
}


const std::vector<GUI::TimelineToolInfo>& GUI::timelineTools()
{
    static const std::vector<TimelineToolInfo> tools = {
        { TimelineView::Tool::Select, ICON_MD_NEAR_ME, "Select", "V",
          "Select, move and trim the clips." },
        { TimelineView::Tool::Blade, ICON_MD_CONTENT_CUT, "Blade", "C",
          "Cut a clip where clicked, with its linked clips. Shift: cut all the tracks." },
        { TimelineView::Tool::Slip, ICON_MD_SWAP_HORIZ, "Slip", "Y",
          "Drag the media inside a clip: the clip stays in place, it shows another part." },
        { TimelineView::Tool::SelectForward, ICON_MD_KEYBOARD_DOUBLE_ARROW_RIGHT, "Select Forward", "A",
          "Select every clip from there to the end, then drag them. Shift: on this track only." },
    };

    return tools;
}


void GUI::addTrack(TrackType type)
{
    // A video track comes with its sound track.
    const Id id = _project.newId();
    const Id soundId = (type == TrackType::Video) ? _project.newId() : InvalidId;
    _project.sequence().addTrack(type, id, soundId);

    commit(type == TrackType::Video ? "Add video track" : (type == TrackType::Subtitle) ? "Add subtitle track" : "Add audio track");
}


void GUI::requestDeleteTrack(Id trackId)
{
    Sequence& sequence = _project.sequence();
    const Track* track = sequence.findTrack(trackId);

    if (!track || _recorder.recording() || _screenRecording) {
        return;
    }

    // With the paired track, a video one goes with its sound.
    size_t clips = track->clips.size();

    if (track->type == TrackType::Video || track->type == TrackType::Sound) {
        const std::vector<Track>& own = sequence.tracks(track->type);
        const size_t index = (size_t)(track - own.data());
        const std::vector<Track>& other = sequence.tracks(track->type == TrackType::Video ? TrackType::Sound : TrackType::Video);

        if (index < other.size()) {
            clips += other[index].clips.size();
        }
    }

    if (clips == 0) {
        deleteTrack(trackId);
    }
    else {
        _confirmDeleteTrack = trackId;
    }
}


void GUI::deleteTrack(Id trackId)
{
    const Track* track = _project.sequence().findTrack(trackId);
    const std::string name = track ? track->name : std::string();

    if (!_project.sequence().removeTrack(trackId)) {
        return;
    }

    // Selected clips of the track gone.
    std::erase_if(_selection, [&](Id id) { return _project.findClip(id) == nullptr; });
    _timeline.clearGap();

    commit("Delete track " + name);
}


void GUI::deleteTrackPopup()
{
    if (_confirmDeleteTrack != InvalidId && !ImGui::IsPopupOpen(tr("Delete Track"))) {
        ImGui::OpenPopup(tr("Delete Track"));
    }

    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

    if (!ImGui::BeginPopupModal(tr("Delete Track"), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }

    const Track* track = _project.sequence().findTrack(_confirmDeleteTrack);

    if (!track) {
        _confirmDeleteTrack = InvalidId;
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }

    const bool paired = track->type == TrackType::Video || track->type == TrackType::Sound;
    ImGui::Text(tr("The track %s is not empty."), track->name.c_str());
    ImGui::TextUnformatted(paired ? tr("Delete it with its clips, and those of its paired video / sound track?") : tr("Delete it with its clips?"));
    ImGui::Spacing();

    if (ImGui::Button(tr(ICON_MD_DELETE " Delete")) || ImGui::IsKeyPressed(ImGuiKey_Enter, false)) {
        deleteTrack(_confirmDeleteTrack);
        _confirmDeleteTrack = InvalidId;
        ImGui::CloseCurrentPopup();
    }

    ImGui::SameLine();

    if (ImGui::Button(tr("Cancel")) || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        _confirmDeleteTrack = InvalidId;
        ImGui::CloseCurrentPopup();
    }

    ImGui::EndPopup();
}


void GUI::timelinePanel()
{
    // The timeline handles the wheel itself.
    const bool open = ImGui::Begin(tr("Timeline###Timeline"), nullptr, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

    if (open) {
        sequenceTabs();

        // Toolbar: tools and snapping, zoom, tracks, help.
        ImGui::PushStyleVarX(ImGuiStyleVar_ItemSpacing, ImGui::GetStyle().ItemInnerSpacing.x);

        for (const TimelineToolInfo& info : timelineTools()) {
            char tooltip[256];
            std::snprintf(tooltip, sizeof(tooltip), "%s (%s)\n%s", tr(info.label), info.shortcut, tr(info.help));

            if (panelButton(info.icon, tooltip, _timeline.tool() == info.tool)) {
                _timeline.setTool(info.tool);
            }
        }

        panelSeparator();

        if (panelButton(ICON_MD_VERTICAL_ALIGN_CENTER, "Snapping (N)", _timeline.snapping())) {
            _timeline.setSnapping(!_timeline.snapping());
        }

        panelSeparator();

        if (panelButton(ICON_MD_ZOOM_OUT, "Zoom Out (Ctrl+-, Ctrl+Wheel)")) {
            _timeline.setZoom(_timeline.zoom() / 1.5f);
        }

        float zoom = _timeline.zoom();
        ImGui::SetNextItemWidth(8.f * ImGui::GetFontSize());

        if (ImGui::SliderFloat("##zoom", &zoom, 0.5f, 2000.f, "", ImGuiSliderFlags_Logarithmic | ImGuiSliderFlags_NoInput)) {
            _timeline.setZoom(zoom);
        }

        ImGui::SetItemTooltip(tr("Zoom"));
        ImGui::SameLine();

        if (panelButton(ICON_MD_ZOOM_IN, "Zoom In (Ctrl++, Ctrl+Wheel)")) {
            _timeline.setZoom(_timeline.zoom() * 1.5f);
        }

        if (panelButton(ICON_MD_FIT_SCREEN, "Show the whole sequence (Ctrl+0)")) {
            _timeline.requestZoomToFit();
        }

        panelSeparator();

        if (panelButton(ICON_MD_LIBRARY_ADD, "Add a video track (with its sound track)")) {
            addTrack(TrackType::Video);
        }

        if (panelButton(ICON_MD_PLAYLIST_ADD, "Add an audio track")) {
            addTrack(TrackType::Audio);
        }

        if (panelButton(ICON_MD_SUBTITLES, "Add a subtitle track")) {
            addTrack(TrackType::Subtitle);
        }

        // The mouse, in a tooltip rather than always on screen.
        ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), ImGui::GetWindowContentRegionMax().x - ImGui::GetFrameHeight()));
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled(ICON_MD_HELP_OUTLINE);
        ImGui::SetItemTooltip(
            tr("Click on the ruler: move the playhead (drag to hear the sound)\n"
            "Click between clips: select the gap (Delete closes it)\n"
            "Drag on an empty area: select the clips in a rectangle\n"
            "Right click: menus of the clips and of the tracks\n"
            "Wheel: scroll in time   Ctrl+Wheel: zoom   Shift+Wheel: scroll the tracks\n"
            "Middle button: move around"));
        ImGui::PopStyleVar();

        if (_recorder.recording() && !_screenRecording) {
            updateLiveWaveform();

            TimelineView::LiveRecording live;
            live.trackId = _recordTrack;
            live.start = _recordStart;
            live.length = mulDivFloor(_recorder.recordedFrames(), TicksPerSecond, audio::Recorder::SampleRate);
            live.waveform = _liveWaveform;
            _timeline.addLiveRecording(std::move(live));
        }

        // Screen recording: the picture, and the microphone beside it.
        if (_screenRecording) {
            TimelineView::LiveRecording live;
            live.trackId = _screenTrack;
            live.start = _screenStart;
            live.length = secondsToTime(_screenRecorder.seconds());
            _timeline.addLiveRecording(live);

            if (!_screenMicrophonePath.empty() && _recorder.recording()) {
                updateLiveWaveform();

                live.trackId = _screenMicrophoneTrack;
                live.length = mulDivFloor(_recorder.recordedFrames(), TicksPerSecond, audio::Recorder::SampleRate);
                live.waveform = _liveWaveform;
                _timeline.addLiveRecording(std::move(live));
            }
        }

        const TimelineView::Result result = _timeline.draw(_project, _playhead, _selection);

        if (result.dropTrack != InvalidId) {
            _dropTrack = result.dropTrack;
            _dropStart = result.dropStart;
        }

        // Moved during playback: it plays again from there once released.
        if (result.seeked) {
            _resumeAfterScrub = _resumeAfterScrub || _playing;
            stopPlayback();
            _audio.scrub(_playhead);
        }

        if (_resumeAfterScrub && !_timeline.scrubbing()) {
            _resumeAfterScrub = false;
            _playing = true;
            _audio.play(_playhead);
        }

        if (result.edited) {
            commit(result.label);
        }

        if (result.openSequence != InvalidId) {
            showSequence(result.openSequence);
        }

        if (result.addTrack) {
            addTrack(result.addTrackType);
        }

        if (result.deleteTrack != InvalidId) {
            requestDeleteTrack(result.deleteTrack);
        }

        deleteTrackPopup();

        if (result.generateSubtitles != InvalidId) {
            _subtitleSourceTrack = result.generateSubtitles;
            _showSubtitleGenerator = true;
            ImGui::SetWindowFocus("###subtitleGenerator");
        }

        if (result.effectsOfTrack != InvalidId) {
            _effectsTrack = result.effectsOfTrack;
            _showEffectsTab = true;

            // The effects of this track, not of the selected clip.
            _selection.clear();
        }

        if (_playing || _recorder.recording() || _screenRecording) {
            _timeline.followPlayhead(_screenRecording ? _screenStart + secondsToTime(_screenRecorder.seconds()) : _playhead);
        }
    }

    ImGui::End();
}


int GUI::mediaInAnalysis() const
{
    int count = 0;

    for (const MediaItem& item : _project.media()) {
        const media::MediaStatus status = _media.state(item.id).status;

        if (status == media::MediaStatus::Probing || status == media::MediaStatus::Decoding) {
            count++;
        }
    }

    return count;
}


bool GUI::startExport()
{
    if (mediaInAnalysis() > 0 || _exportSettings.path.empty()) {
        return false;
    }

    stopPlayback();

    ExportContext context;
    context.device = &device();
    context.pipelineCache = pipelineCache();
    context.queueFamily = device().queueFamilyIdx(Queue::Transfer);
    context.queue = backgroundQueue();
    context.queueMutex = &queueMutex(backgroundQueue());
    context.hwDevice = _hwDevice ? _hwDevice->context() : nullptr;
    context.vulkanEncode = hasVideoEncodeQueue() && _hwDevice;
    context.defaultFont = _defaultFontData;

    // The sound of each media, all its streams together (-1) and each one.
    std::map<std::pair<Id, int>, std::shared_ptr<const media::AudioBuffer>> audio;

    for (const MediaItem& item : _project.media()) {
        for (int stream = -1; stream < (int)std::max<size_t>(1, item.audioStreams.size()); stream++) {
            if (std::shared_ptr<const media::AudioBuffer> buffer = _media.audio(item.id, stream)) {
                audio[{ item.id, stream }] = buffer;
            }
        }
    }

    // The plugins as set in their editors, in new instances for the export
    // thread.
    _effects.saveStates(_project);

    // The subtitles: a file beside the video, or none in the picture.
    Project exported = _project;

    if (!subtitleCues(_project.sequence()).empty()) {
        if (_exportSubtitles) {
            std::filesystem::path srt = _exportSettings.path;
            exportSubtitles(srt.replace_extension(".srt"));
        }
        else {
            for (Track& track : exported.sequence().subtitleTracks) track.muted = true;
        }
    }

    _exporter = std::make_unique<Exporter>(std::move(context), std::move(exported), std::move(audio),
        EffectRack::createForExport(_project), _exportSettings);

    return true;
}


void GUI::exportAndQuit(const std::filesystem::path& path, ExportSettings::Codec codec, uint32_t height, int frameRate, float lufs)
{
    _exportSettings.normalizeLoudness = lufs < 0.f;
    _exportSettings.targetLufs = (lufs < 0.f) ? lufs : _exportSettings.targetLufs;
    _exportSettings.path = path;
    _exportSettings.codec = codec;
    _exportSettings.height = height;
    _exportSettings.frameRate = { frameRate, 1 };
    _autoExport = true;
}


void GUI::sequenceTabs()
{
    if (!ImGui::BeginTabBar("##sequences", ImGuiTabBarFlags_FittingPolicyScroll | ImGuiTabBarFlags_Reorderable)) {
        return;
    }

    Id show = InvalidId;
    Id remove = InvalidId;
    Id selectedTab = InvalidId;

    for (const Sequence* sequence : _project.sequences()) {
        const bool current = sequence->id == _project.sequence().id;
        const ImGuiTabItemFlags flags = (_selectTab == sequence->id) ? ImGuiTabItemFlags_SetSelected : 0;
        const std::string label = std::string(ICON_MD_VIDEO_LIBRARY " ") + sequence->name + "###sequence" + std::to_string(sequence->id);

        const bool selected = ImGui::BeginTabItem(label.c_str(), nullptr, flags);

        // Dragged into the timeline: nested.
        if (ImGui::BeginDragDropSource()) {
            ImGui::SetDragDropPayload(TimelineView::SequencePayload, &sequence->id, sizeof(sequence->id));
            ImGui::Text(ICON_MD_VIDEO_LIBRARY " %s", sequence->name.c_str());
            ImGui::EndDragDropSource();
        }

        ImGui::SetItemTooltip(tr("Drag into the timeline of another sequence to nest it"));

        if (ImGui::BeginPopupContextItem()) {
            if (ImGui::MenuItem(tr(ICON_MD_EDIT " Rename"))) {
                _renamingSequence = sequence->id;
                _sequenceName = sequence->name;
            }

            if (ImGui::MenuItem(tr(ICON_MD_DELETE " Delete Sequence"), nullptr, false, _project.sequences().size() > 1)) {
                remove = sequence->id;
            }

            ImGui::EndPopup();
        }

        // Chosen by the user (not while a tab is being selected by the
        // application).
        if (selected) {
            selectedTab = sequence->id;

            if (!current && _selectTab == InvalidId) {
                show = sequence->id;
            }

            ImGui::EndTabItem();
        }
    }

    if (selectedTab == _project.sequence().id) {
        _selectTab = InvalidId;
    }

    // A new sequence, shown.
    if (ImGui::TabItemButton(ICON_MD_ADD, ImGuiTabItemFlags_Trailing | ImGuiTabItemFlags_NoTooltip)) {
        const Id id = _project.addSequence("Sequence " + std::to_string(_project.sequences().size() + 1));
        commit("Add sequence");
        showSequence(id);
    }

    ImGui::SetItemTooltip(tr("New sequence"));
    ImGui::EndTabBar();

    if (show != InvalidId) {
        showSequence(show);
    }

    if (remove != InvalidId) {
        if (_project.removeSequence(remove)) {
            _selection.clear();
            _selectTab = _project.sequence().id;
            commit("Delete sequence");
        }
        else {
            setErrorMessage(tr("This sequence is nested in another one: remove it from there first."));
        }
    }

    // Renamed in a small window.
    if (_renamingSequence != InvalidId) {
        ImGui::OpenPopup(tr("Rename Sequence"));
    }

    if (ImGui::BeginPopupModal(tr("Rename Sequence"), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        if (ImGui::IsWindowAppearing()) {
            ImGui::SetKeyboardFocusHere();
        }

        const bool entered = ImGui::InputText("##name", &_sequenceName, ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);

        if ((entered || ImGui::Button(tr("OK"))) && !_sequenceName.empty()) {
            const Id id = _renamingSequence;
            const Id current = _project.sequence().id;

            // Renamed where it is kept.
            _project.switchSequence(id);
            _project.sequence().name = _sequenceName;
            _project.switchSequence(current);

            _renamingSequence = InvalidId;
            commit("Rename sequence");
            ImGui::CloseCurrentPopup();
        }

        ImGui::SameLine();

        if (ImGui::Button(tr("Cancel")) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            _renamingSequence = InvalidId;
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }
}


void GUI::showSequence(Id id)
{
    if (id == _project.sequence().id || !_project.findSequence(id)) {
        _selectTab = id;
        return;
    }

    stopPlayback();
    endTextEditing();
    _sequencePlayheads[_project.sequence().id] = _playhead;

    _project.switchSequence(id);
    _selection.clear();
    _playhead = _sequencePlayheads.count(id) ? _sequencePlayheads[id] : 0;
    _selectTab = id;

    // Not an edit: the history keeps the sequences, not which one is shown.
    projectChanged();
}


void GUI::keyframesPanel()
{
    // Beside the timeline, behind it until chosen.
    if (ImGuiWindow* timeline = ImGui::FindWindowByName("###Timeline"); timeline && timeline->DockId) {
        ImGui::SetNextWindowDockID(timeline->DockId, ImGuiCond_FirstUseEver);
    }

    if (std::exchange(_focusKeyframes, false)) {
        ImGui::SetNextWindowFocus();
    }

    if (ImGui::Begin(tr("Keyframes###Keyframes"), nullptr, ImGuiWindowFlags_NoFocusOnAppearing)) {
        const KeyframeEditor::Result result = _keyframes.draw(_project, _selection, _playhead);

        if (result.seeked) {
            stopPlayback();
            _audio.scrub(_playhead);
        }

        if (!result.edit.empty()) {
            commit(result.edit);
        }
    }

    ImGui::End();
}


void GUI::projectPropertiesPopup()
{
    if (std::exchange(_requestProjectProperties, false)) {
        ImGui::OpenPopup(tr("Project Properties"));
    }

    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(26.f * ImGui::GetFontSize(), 0.f), ImGuiCond_Appearing);

    if (!ImGui::BeginPopupModal(tr("Project Properties"), nullptr, ImGuiWindowFlags_NoSavedSettings)) {
        return;
    }

    std::string edit;
    _inspector.sequenceSection(_project, edit);
    _inspector.subtitleStyleSection(_project, edit);

    if (!edit.empty()) {
        commit(edit);
    }

    ImGui::Spacing();
    ImGui::TextDisabled(tr("The export can use another size or frame rate."));
    ImGui::Spacing();

    if (ImGui::Button(tr("Close"), ImVec2(8.f * ImGui::GetFontSize(), 0.f)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        ImGui::CloseCurrentPopup();
    }

    ImGui::EndPopup();
}


void GUI::exportPopup()
{
    if (std::exchange(_requestExportPopup, false)) {
        if (_exportPathText.empty()) {
            // Next to the project, named after it.
            std::filesystem::path path = _projectPath.empty() ? std::filesystem::path("export.mp4") : _projectPath;
            path.replace_extension(".mp4");
            _exportPathText = pathToUtf8(path);
        }

        ImGui::OpenPopup(tr("Export Video"));
    }

    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(36.f * ImGui::GetFontSize(), 0.f), ImGuiCond_Appearing);

    if (!ImGui::BeginPopupModal(tr("Export Video"), nullptr, ImGuiWindowFlags_NoSavedSettings)) {
        return;
    }

    const float labelWidth = 7.f * ImGui::GetFontSize();

    if (_exporter) {
        // Running, or done.
        const Exporter& exporter = *_exporter;
        const std::string target = pathToUtf8(exporter.settings().path.filename());

        if (!exporter.done()) {
            ImGui::Text(tr("Exporting %s"), target.c_str());

            if (!exporter.encoder().empty()) {
                ImGui::TextDisabled(tr("Encoder: %s"), exporter.encoder().c_str());
            }

            ImGui::ProgressBar(exporter.progress(), ImVec2(-FLT_MIN, 0.f));

            if (ImGui::Button(tr(ICON_MD_CLOSE " Cancel"))) {
                _exporter->cancel();
            }
        }
        else {
            if (exporter.succeeded()) {
                ImGui::TextColored(ImVec4(0.4f, 0.85f, 0.5f, 1.f), tr(ICON_MD_CHECK_CIRCLE " %s exported"), target.c_str());
                ImGui::TextDisabled(tr("Encoder: %s"), exporter.encoder().c_str());

                if (const std::string loudness = exporter.loudnessReport(); !loudness.empty()) {
                    ImGui::TextDisabled("%s", loudness.c_str());
                }
            }
            else if (exporter.cancelled()) {
                ImGui::TextUnformatted(tr("Export cancelled."));
            }
            else {
                ImGui::TextColored(ImVec4(0.9f, 0.35f, 0.3f, 1.f), tr(ICON_MD_WARNING " Export failed"));
                ImGui::TextWrapped("%s", exporter.error().c_str());
            }

            if (exporter.succeeded() && ImGui::Button(tr(ICON_MD_FOLDER_OPEN " Show in Folder"))) {
                const std::u8string folder = exporter.settings().path.parent_path().generic_u8string();
                SDL_OpenURL(("file:///" + std::string(folder.begin(), folder.end())).c_str());
            }

            if (exporter.succeeded()) {
                ImGui::SameLine();
            }

            if (ImGui::Button(tr("Close"))) {
                _exporter.reset();
                ImGui::CloseCurrentPopup();
            }
        }

        ImGui::EndPopup();
        return;
    }

    // Settings
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled(tr("File"));
    ImGui::SameLine(labelWidth);

    const float browseWidth = ImGui::CalcTextSize(tr("Browse...")).x + 2.f * ImGui::GetStyle().FramePadding.x;
    ImGui::SetNextItemWidth(-browseWidth - ImGui::GetStyle().ItemSpacing.x);
    ImGui::InputText("##path", &_exportPathText);
    ImGui::SameLine();

    if (ImGui::Button(tr("Browse..."))) {
        SDL_ShowSaveFileDialog(GUI::onExportFile, this, window(), exportFilters, SDL_arraysize(exportFilters), nullptr);
    }

    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled(tr("Codec"));
    ImGui::SameLine(labelWidth);

    const char* codecs[] = { tr("H.264 (most compatible)"), tr("H.265 / HEVC"), tr("AV1") };
    int codec = (int)_exportSettings.codec;
    ImGui::SetNextItemWidth(-FLT_MIN);

    if (ImGui::Combo("##codec", &codec, codecs, IM_ARRAYSIZE(codecs))) {
        _exportSettings.codec = (ExportSettings::Codec)codec;
    }

    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled(tr("Quality"));
    ImGui::SameLine(labelWidth);
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::SliderFloat("##bitrate", &_exportSettings.bitrateMbps, 2.f, 100.f, "%.0f Mbit/s", ImGuiSliderFlags_Logarithmic);

    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled(tr("Audio"));
    ImGui::SameLine(labelWidth);
    ImGui::Checkbox(tr("AAC, 192 kbit/s"), &_exportSettings.audio);

    // Loudness: the usual targets, or any.
    ImGui::BeginDisabled(!_exportSettings.audio);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled(tr("Loudness"));
    ImGui::SameLine(labelWidth);
    ImGui::Checkbox(tr("Normalize to##loudness"), &_exportSettings.normalizeLoudness);
    ImGui::SetItemTooltip(tr("The whole sound is measured first (EBU R128), then brought to this loudness,\n"
        "its peaks kept under -1 dB."));
    ImGui::SameLine();
    ImGui::BeginDisabled(!_exportSettings.normalizeLoudness);

    struct LoudnessTarget
    {
        float lufs;
        const char* label;
    };

    const LoudnessTarget targets[] = {
        { -14.f, tr("-14 LUFS  (YouTube, Spotify)") },
        { -16.f, tr("-16 LUFS  (podcasts, Apple)") },
        { -23.f, tr("-23 LUFS  (broadcast, EBU R128)") },
    };

    char targetLabel[48];
    std::snprintf(targetLabel, sizeof(targetLabel), "%.1f LUFS", _exportSettings.targetLufs);

    for (const LoudnessTarget& target : targets) {
        if (target.lufs == _exportSettings.targetLufs) std::snprintf(targetLabel, sizeof(targetLabel), "%s", tr(target.label));
    }

    ImGui::SetNextItemWidth(-FLT_MIN);

    if (ImGui::BeginCombo("##targetLufs", targetLabel)) {
        for (const LoudnessTarget& target : targets) {
            if (ImGui::Selectable(tr(target.label), target.lufs == _exportSettings.targetLufs)) {
                _exportSettings.targetLufs = target.lufs;
            }
        }

        ImGui::Separator();
        ImGui::SetNextItemWidth(8.f * ImGui::GetFontSize());
        ImGui::DragFloat(tr("Other##otherLufs"), &_exportSettings.targetLufs, 0.1f, -40.f, -5.f, tr("%.1f LUFS"), ImGuiSliderFlags_AlwaysClamp);
        ImGui::EndCombo();
    }

    ImGui::EndDisabled();
    ImGui::EndDisabled();

    // Subtitles: in the picture, and in a file beside it.
    if (!subtitleCues(_project.sequence()).empty()) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled(tr("Subtitles"));
        ImGui::SameLine(labelWidth);
        ImGui::Checkbox(tr("In the picture, and a .srt file beside the video"), &_exportSubtitles);
    }

    // Size and rate, those of the sequence by default; the shape stays the
    // one of the sequence.
    const Sequence& sequence = _project.sequence();
    auto widthFor = [&](uint32_t height) {
        return (uint32_t)std::lround((double)height * sequence.width / std::max(1u, sequence.height)) & ~1u;
    };

    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled(tr("Size"));
    ImGui::SameLine(labelWidth);
    ImGui::SetNextItemWidth(-FLT_MIN);

    const uint32_t heights[] = { 0, 720, 1080, 1440, 2160 };
    const char* heightNames[] = { "", "720p", "1080p", "1440p", "4K (2160p)" };
    auto sizeLabel = [&](size_t i) {
        const uint32_t h = heights[i] ? heights[i] : sequence.height;
        const std::string size = std::to_string(widthFor(h)) + " x " + std::to_string(h & ~1u);
        return heights[i] ? std::string(heightNames[i]) + "  (" + size + ")" : tr("Sequence  (") + size + ")";
    };

    size_t currentSize = 0;

    for (size_t i = 0; i < IM_ARRAYSIZE(heights); i++) {
        if (heights[i] == _exportSettings.height) currentSize = i;
    }

    if (ImGui::BeginCombo("##size", sizeLabel(currentSize).c_str())) {
        for (size_t i = 0; i < IM_ARRAYSIZE(heights); i++) {
            if (ImGui::Selectable(sizeLabel(i).c_str(), i == currentSize)) {
                _exportSettings.height = heights[i];
            }
        }

        ImGui::EndCombo();
    }

    if (_exportSettings.height > sequence.height) {
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + labelWidth);
        ImGui::TextDisabled(tr("Larger than the sequence: the videos smaller than this are enlarged."));
    }

    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled(tr("Frame rate"));
    ImGui::SameLine(labelWidth);
    ImGui::SetNextItemWidth(-FLT_MIN);

    const Rational rates[] = { { 0, 1 }, { 24, 1 }, { 25, 1 }, { 30000, 1001 }, { 30, 1 }, { 50, 1 }, { 60000, 1001 }, { 60, 1 } };
    auto rateLabel = [&](const Rational& rate) {
        char text[48];

        if (rate.num == 0) {
            std::snprintf(text, sizeof(text), tr("Sequence  (%.3g fps)"), sequence.frameRate.toDouble());
        }
        else {
            std::snprintf(text, sizeof(text), "%.3g fps", rate.toDouble());
        }

        return std::string(text);
    };

    if (ImGui::BeginCombo("##frameRate", rateLabel(_exportSettings.frameRate).c_str())) {
        for (const Rational& rate : rates) {
            const bool selected = rate.num == _exportSettings.frameRate.num && rate.den == _exportSettings.frameRate.den;

            if (ImGui::Selectable(rateLabel(rate).c_str(), selected)) {
                _exportSettings.frameRate = rate;
            }
        }

        ImGui::EndCombo();
    }

    ImGui::TextDisabled(tr("%s long"), formatTimecode(sequence.duration(), sequence.frameRate).c_str());

    if (hasVideoEncodeQueue()) {
        ImGui::TextDisabled(tr(ICON_MD_BOLT " Encoded on the GPU with Vulkan Video when the codec allows it"));
    }

    const int analyzing = mediaInAnalysis();

    if (analyzing > 0) {
        ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.3f, 1.f), tr("%d media still being analyzed..."), analyzing);
    }

    ImGui::Separator();

    ImGui::BeginDisabled(analyzing > 0 || _exportPathText.empty() || sequence.duration() <= 0);

    if (ImGui::Button(tr(ICON_MD_MOVIE_CREATION " Export"))) {
        _exportSettings.path = std::filesystem::path(std::u8string(_exportPathText.begin(), _exportPathText.end()));
        startExport();
    }

    ImGui::EndDisabled();
    ImGui::SameLine();

    if (ImGui::Button(tr("Cancel")) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        ImGui::CloseCurrentPopup();
    }

    ImGui::EndPopup();
}


void SDLCALL GUI::onExportFile(void* userdata, const char* const* filelist, int filter)
{
    if (!userdata || !filelist || !filelist[0]) {
        return;
    }

    GUI* pGUI = static_cast<GUI*>(userdata);
    std::filesystem::path filepath = std::filesystem::u8path(filelist[0]);

    if (filepath.extension().empty() && filter >= 0 && filter < (int)SDL_arraysize(exportFilters)) {
        filepath += "." + std::string(exportFilters[filter].pattern);
    }

    std::scoped_lock lock(pGUI->_dialogMutex);
    pGUI->_dialogExportPath = filepath;
}


void GUI::openEffectEditor(Id slotId)
{
    if (audio::Vst3Plugin* plugin = _effects.plugin(slotId)) {
        plugin->openEditor();
    }

    for (const auto* tracks : { &_project.sequence().soundTracks, &_project.sequence().audioTracks }) {
        for (const Track& track : *tracks) {
            for (const EffectSlot& slot : track.effects) {
                if (slot.id == slotId) {
                    _effectsTrack = track.id;
                    _showEffectsTab = true;
                }
            }
        }
    }
}


void GUI::eventSdl(const SDL_Event& event)
{
    switch (event.type) {
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
        case SDL_EVENT_WINDOW_RESIZED:
            _effects.handleEvent(event);
            break;
        default:
            break;
    }
}


void GUI::effectsTab()
{
    // The selected clip and its track; else the track chosen last.
    Sequence& sequence = _project.sequence();
    Clip* clip = _selection.empty() ? nullptr : _project.findClip(_selection.front());
    const Track* trackOfClip = clip ? _project.findTrackOfClip(clip->id) : nullptr;
    Track* clipTrack = trackOfClip ? _project.findTrack(trackOfClip->id) : nullptr;

    if (clipTrack) {
        _effectsTrack = clipTrack->id;
    }

    Track* track = _project.findTrack(_effectsTrack);

    // Any track can be picked.
    ImGui::SetNextItemWidth(-FLT_MIN);

    auto count = [](const Track& t) { return t.effects.size() + t.videoEffects.size(); };

    if (ImGui::BeginCombo("##effectsTrack", track ? (std::string(tr(ICON_MD_TUNE " Track ")) + track->name).c_str() : tr("Choose a track..."))) {
        for (const auto* tracks : { &sequence.videoTracks, &sequence.soundTracks, &sequence.audioTracks, &sequence.subtitleTracks }) {
            for (const Track& t : *tracks) {
                const std::string label = t.name + (count(t) == 0 ? "" : "  (" + std::to_string(count(t)) + ")");

                if (ImGui::Selectable(label.c_str(), track && t.id == track->id)) {
                    _effectsTrack = t.id;
                    _selection.clear();
                    clip = nullptr;
                }
            }
        }

        ImGui::EndCombo();
    }

    track = _project.findTrack(_effectsTrack);

    if (!track) {
        ImGui::TextDisabled(tr("Select a clip, or choose a track."));
        return;
    }

    const bool video = track->type == TrackType::Video;
    const Time localTime = clip ? std::clamp<Time>(_playhead - clip->start, 0, clip->duration) : 0;
    std::string edit;

    // The clip first: its effects come before those of its track.
    if (clip && clip->kind == ClipKind::Media) {
        ImGui::SeparatorText(video ? tr(ICON_MD_MOVIE " Clip") : tr(ICON_MD_AUDIOTRACK " Clip"));

        if (video) {
            _inspector.videoEffectsSection(_project, clip->videoEffects, localTime, edit);
        }
        else {
            audioEffectChain(clip->effects, "clip");
        }
    }

    ImGui::SeparatorText((std::string(tr(ICON_MD_TUNE " Track ")) + track->name).c_str());
    ImGui::TextDisabled(video ? tr("On each clip of the track, after its own.") : tr("Applied in order, to the whole track."));

    if (video) {
        // Blending of the clips of the track with the tracks below.
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%s", tr("Blend"));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-FLT_MIN);

        if (ImGui::BeginCombo("##trackBlend", tr(blendModeName(track->blend)))) {
            for (int mode = 0; mode < (int)BlendMode::Count; mode++) {
                if (ImGui::Selectable(tr(blendModeName((BlendMode)mode)), (int)track->blend == mode)) {
                    track->blend = (BlendMode)mode;
                    edit = "Change blending";
                }
            }

            ImGui::EndCombo();
        }

        // Keyframes of track effects: in the time of the sequence.
        _inspector.videoEffectsSection(_project, track->videoEffects, _playhead, edit);
    }
    else {
        audioEffectChain(track->effects, "track");
    }

    // A video track: the sound of the clip and its sound track too, so that
    // the effects of a video are all in one place.
    if (video) {
        const size_t index = (size_t)(track - sequence.videoTracks.data());
        Track* soundTrack = (index < sequence.soundTracks.size()) ? &sequence.soundTracks[index] : nullptr;
        Clip* sound = nullptr;

        if (soundTrack && clip && clip->linkGroup != InvalidId) {
            for (Clip& c : soundTrack->clips) {
                if (c.linkGroup == clip->linkGroup) sound = &c;
            }
        }

        if (sound) {
            ImGui::SeparatorText(tr(ICON_MD_AUDIOTRACK " Sound of the Clip"));
            audioEffectChain(sound->effects, "soundClip");
        }

        if (soundTrack) {
            ImGui::SeparatorText((std::string(tr(ICON_MD_TUNE " Sound Track ")) + soundTrack->name).c_str());
            audioEffectChain(soundTrack->effects, "soundTrack");
        }
    }

    if (!edit.empty()) {
        commit(edit);
    }
}


void GUI::audioEffectChain(std::vector<EffectSlot>& slots, const char* id)
{
    ImGui::PushID(id);

    // The effects installed, looked for once in the background.
    if (!_pluginScanner.started()) {
        _pluginScanner.start();
    }

    Id removed = InvalidId;
    int moveUp = -1;

    for (int i = 0; i < (int)slots.size(); i++) {
        EffectSlot& slot = slots[i];
        ImGui::PushID((int)slot.id);

        bool active = !slot.bypass;

        if (ImGui::Checkbox("##active", &active)) {
            slot.bypass = !active;
            commit(active ? "Enable effect" : "Bypass effect");
        }

        ImGui::SetItemTooltip(tr("On / bypassed"));
        ImGui::SameLine();

        audio::Vst3Plugin* plugin = _effects.plugin(slot.id);
        const std::string error = _effects.error(slot.id);

        ImGui::AlignTextToFramePadding();

        if (!error.empty()) {
            ImGui::TextColored(ImVec4(0.9f, 0.35f, 0.3f, 1.f), ICON_MD_WARNING " %s", slot.name.c_str());
            ImGui::SetItemTooltip("%s", error.c_str());
        }
        else {
            ImGui::TextUnformatted(slot.name.c_str());
        }

        ImGui::SameLine(ImGui::GetContentRegionMax().x - 3.f * ImGui::GetFrameHeight() - 2.f * ImGui::GetStyle().ItemSpacing.x);

        ImGui::BeginDisabled(!plugin || !plugin->hasEditor());

        if (ImGui::Button(ICON_MD_OPEN_IN_NEW)) {
            if (!plugin->openEditor()) {
                setErrorMessage(slot.name + tr(": the plugin has no editor that can be shown."));
            }
        }

        ImGui::EndDisabled();
        ImGui::SetItemTooltip(tr("Open the plugin editor"));
        ImGui::SameLine();

        ImGui::BeginDisabled(i == 0);

        if (ImGui::Button(ICON_MD_ARROW_UPWARD)) {
            moveUp = i;
        }

        ImGui::EndDisabled();
        ImGui::SetItemTooltip(tr("Earlier in the chain"));
        ImGui::SameLine();

        if (ImGui::Button(ICON_MD_DELETE)) {
            removed = slot.id;
        }

        ImGui::SetItemTooltip(tr("Remove"));
        ImGui::PopID();
    }

    if (slots.empty()) {
        ImGui::TextDisabled(tr("No effect yet."));
    }

    if (moveUp > 0) {
        _effects.saveStates(_project);
        std::swap(slots[moveUp], slots[moveUp - 1]);
        commit("Reorder effects");
    }

    if (removed != InvalidId) {
        _effects.saveStates(_project);
        std::erase_if(slots, [&](const EffectSlot& s) { return s.id == removed; });
        commit("Remove effect");
    }

    ImGui::Separator();
    ImGui::SetNextItemWidth(-FLT_MIN);

    if (ImGui::BeginCombo("##add", tr(ICON_MD_ADD " Add an effect..."), ImGuiComboFlags_HeightLargest)) {
        const std::vector<audio::PluginClass> plugins = _pluginScanner.classes();

        if (_pluginScanner.scanning()) {
            const std::string current = _pluginScanner.current();
            ImGui::TextDisabled(tr(ICON_MD_HOURGLASS_TOP " Looking for plugins... %s"), current.c_str());
        }
        else if (plugins.empty()) {
            ImGui::TextDisabled(tr("No VST3 effect found in the standard folders."));
        }

        // Filter, on the name and the vendor.
        if (ImGui::IsWindowAppearing()) {
            ImGui::SetKeyboardFocusHere();
        }

        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputTextWithHint("##filter", tr(ICON_MD_SEARCH " Search"), _pluginFilter, sizeof(_pluginFilter));

        auto lower = [](std::string text) {
            std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return (char)std::tolower(c); });
            return text;
        };

        const std::string filter = lower(_pluginFilter);

        for (const audio::PluginClass& plugin : plugins) {
            if (!filter.empty() && lower(plugin.name + " " + plugin.vendor).find(filter) == std::string::npos) {
                continue;
            }

            ImGui::PushID((plugin.path + plugin.classId).c_str());
            const bool chosen = ImGui::Selectable(plugin.name.c_str());

            if (!plugin.vendor.empty()) {
                ImGui::SameLine();
                ImGui::TextDisabled("%s", plugin.vendor.c_str());
            }

            ImGui::PopID();

            if (chosen) {
                EffectSlot slot;
                slot.id = _project.newId();
                slot.path = plugin.path;
                slot.classId = plugin.classId;
                slot.name = plugin.name;

                // Effects stay in the project, their states with them.
                _effects.saveStates(_project);
                slots.push_back(slot);
                commit("Add effect");

                // Opened at once, to set it.
                if (audio::Vst3Plugin* plugin = _effects.plugin(slot.id)) {
                    plugin->openEditor();
                }
                else if (!_effects.error(slot.id).empty()) {
                    setErrorMessage(slot.name + ": " + _effects.error(slot.id));
                }
            }
        }

        ImGui::Separator();
        ImGui::BeginDisabled(_pluginScanner.scanning());

        if (ImGui::Selectable(tr(ICON_MD_REFRESH " Look for plugins again"), false, ImGuiSelectableFlags_DontClosePopups)) {
            _pluginScanner.start(true);
        }

        ImGui::EndDisabled();

        ImGui::EndCombo();
    }

    ImGui::PopID();
}


void GUI::errorPopup()
{
    bool open = false;

    {
        std::scoped_lock lock(_errorMutex);
        open = std::exchange(_hasErrorMessage, false);
    }

    if (open) {
        ImGui::OpenPopup(tr("Error"));
    }

    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

    if (ImGui::BeginPopupModal(tr("Error"), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        {
            std::scoped_lock lock(_errorMutex);
            ImGui::TextUnformatted(_errorMessage.c_str());
        }

        if (ImGui::Button(tr("OK"))) {
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }
}


void GUI::unsavedChangesPopup()
{
    if (std::exchange(_requestUnsavedChangesPopup, false)) {
        ImGui::OpenPopup(tr("Unsaved Changes"));
    }

    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

    if (ImGui::BeginPopupModal(tr("Unsaved Changes"), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted(_pendingAction == PendingAction::Quit
            ? tr("Save the changes to the project before closing?")
            : tr("Save the changes to the current project first?"));
        ImGui::Spacing();

        if (ImGui::Button(tr(ICON_MD_SAVE " Save"))) {
            ImGui::CloseCurrentPopup();
            _actionAfterSave = true;
            requestSaveFile();
        }

        ImGui::SameLine();

        if (ImGui::Button(tr("Don't Save"))) {
            ImGui::CloseCurrentPopup();
            doPendingAction(std::exchange(_pendingAction, PendingAction::None), _pendingPath);
        }

        ImGui::SameLine();

        if (ImGui::Button(tr("Cancel")) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            ImGui::CloseCurrentPopup();
            _pendingAction = PendingAction::None;
        }

        ImGui::EndPopup();
    }
}


void GUI::setErrorMessage(const std::string& message)
{
    std::scoped_lock lock(_errorMutex);
    _errorMessage = message;
    _hasErrorMessage = true;
}


void GUI::updateWindowTitle()
{
    std::string title = _projectPath.empty() ? std::string(tr("Untitled")) : pathToUtf8(_projectPath.stem());

    if (_history.modified()) {
        title += " *";
    }

    if (title != _windowTitle) {
        _windowTitle = title;
        SDL_SetWindowTitle(window(), (title + " - " APP_NAME_STR).c_str());
    }
}


// ----------------------------------------------------------------------------
// Commands
// ----------------------------------------------------------------------------

void GUI::quit()
{
    confirmThen(PendingAction::Quit);
}


void GUI::confirmThen(PendingAction action, const std::filesystem::path& path)
{
    endTextEditing();

    if (!_history.modified()) {
        doPendingAction(action, path);
        return;
    }

    _pendingAction = action;
    _pendingPath = path;
    _actionAfterSave = false;
    _requestUnsavedChangesPopup = true;
}


void GUI::doPendingAction(PendingAction action, const std::filesystem::path& path)
{
    switch (action) {
        case PendingAction::Quit:        GUIBase::quit(); break;
        case PendingAction::NewProject:  newProject(); break;
        case PendingAction::OpenProject: openProject(path); break;
        default: break;
    }
}


void GUI::newProject()
{
    stopPlayback();

    replaceProject(Project(), {});
}


TimelineView::Thumbnails GUI::thumbnailsOf(Id mediaId)
{
    const MediaItem* item = _project.findMedia(mediaId);

    // Once probed: the length places the pictures.
    if (!item || !item->probed || item->type == MediaType::Audio) {
        return {};
    }

    ThumbnailTexture& entry = _thumbnailTextures[item->path];

    if (!entry.strip) {
        entry.strip = _thumbnails.get(item->path, item->duration, item->type == MediaType::Image);
    }

    media::ThumbnailStrip& strip = *entry.strip;

    if (!entry.texture) {
        entry.texture = IM_NEW(ImTextureData)();
        entry.texture->Create(ImTextureFormat_RGBA32, strip.atlasWidth(), strip.atlasHeight());
        ImGui::RegisterUserTexture(entry.texture);
    }

    TimelineView::Thumbnails thumbnails;

    {
        std::scoped_lock lock(strip.mutex);

        // New tiles: the whole atlas to the GPU (a few MB at most, while
        // the strip is being made).
        if (strip.dirty) {
            strip.dirty = false;
            std::memcpy(entry.texture->GetPixels(), strip.rgba.data(), std::min(strip.rgba.size(), (size_t)entry.texture->GetSizeInBytes()));

            if (entry.texture->Status == ImTextureStatus_OK || entry.texture->Status == ImTextureStatus_WantUpdates) {
                const ImTextureRect all = { 0, 0, (unsigned short)strip.atlasWidth(), (unsigned short)strip.atlasHeight() };
                entry.texture->UpdateRect = all;
                entry.texture->Updates.resize(0);
                entry.texture->Updates.push_back(all);
                entry.texture->SetStatus(ImTextureStatus_WantUpdates);
            }
        }

        thumbnails.ready = strip.ready;
    }

    thumbnails.texture = entry.texture->GetTexRef();
    thumbnails.count = strip.count;
    thumbnails.interval = strip.interval;
    thumbnails.image = item->type == MediaType::Image;
    thumbnails.columns = media::ThumbnailStrip::Columns;
    thumbnails.tileU = (float)media::ThumbnailStrip::TileWidth / (float)strip.atlasWidth();
    thumbnails.tileV = (float)media::ThumbnailStrip::TileHeight / (float)strip.atlasHeight();

    return thumbnails;
}


void GUI::matchSequenceToVideo()
{
    // The media known when the project opens stay as they are.
    if (!_matchedMediaSeeded) {
        _matchedMediaSeeded = true;
        _matchedMedia.clear();

        for (const MediaItem& item : _project.media()) {
            if (item.probed) _matchedMedia.insert(item.id);
        }

        return;
    }

    Sequence& sequence = _project.sequence();

    for (const MediaItem& item : _project.media()) {
        if (!item.probed || !_matchedMedia.insert(item.id).second || item.type != MediaType::Video || item.width == 0 || item.height == 0) {
            continue;
        }

        // Only while the sequence shows nothing else: no placement to adapt.
        const bool alone = std::all_of(sequence.videoTracks.begin(), sequence.videoTracks.end(), [&](const Track& track) {
            return std::all_of(track.clips.begin(), track.clips.end(), [&](const Clip& clip) {
                return clip.kind == ClipKind::Media && clip.mediaId == item.id;
            });
        });

        if (!alone) {
            continue;
        }

        const bool larger = item.width > sequence.width || item.height > sequence.height;
        const bool faster = item.frameRate.toDouble() > sequence.frameRate.toDouble() + 0.01;

        if (!larger && !faster) {
            continue;
        }

        if (larger) {
            sequence.width = item.width & ~1u;
            sequence.height = item.height & ~1u;
        }

        if (faster) {
            sequence.frameRate = item.frameRate;
        }

        SDL_Log("Sequence matched to %s: %u x %u, %.3f fps", item.name().c_str(), sequence.width, sequence.height, sequence.frameRate.toDouble());
        commit("Match the sequence to " + item.name());
        break;
    }
}


void GUI::replaceProject(Project project, const std::filesystem::path& path)
{
    endTextEditing();
    _matchedMediaSeeded = false;
    _sequencePlayheads.clear();
    _selectTab = project.sequence().id;

    // The ids of the new project restart: nothing analyzed or decoded for
    // the previous one may be taken for its media.
    _media.clear();
    _loggedVideoMedia.clear();
    _resetDecoders = true;
    _effectsTrack = InvalidId;
    _draggedLayer = InvalidId;

    _project = std::move(project);
    _projectPath = path;
    _selection.clear();
    _selectedMedia = InvalidId;
    _playhead = 0;
    _history.reset(_project);
    projectChanged();
}


void GUI::openProject(const std::filesystem::path& filepath)
{
    try {
        Project project;
        project.load(filepath);

        stopPlayback();

        replaceProject(std::move(project), filepath);
        _timeline.requestZoomToFit();
    }
    catch (const std::exception& e) {
        setErrorMessage(std::string(tr("Could not open the project:\n")) + e.what());
    }
}


bool GUI::saveProject(const std::filesystem::path& filepath)
{
    // Parameters changed in the plugin editors.
    _effects.saveStates(_project);

    try {
        _project.save(filepath);
        _projectPath = filepath;
        _history.markSaved();
        return true;
    }
    catch (const std::exception& e) {
        setErrorMessage(std::string(tr("Could not save the project:\n")) + e.what());
        return false;
    }
}


void GUI::requestImportMedia()
{
    SDL_ShowOpenFileDialog(
        GUI::onFileImport,          // callback
        this,                       // userdata
        window(),                   // parent window
        mediaFilters,
        SDL_arraysize(mediaFilters),
        nullptr,                    // default location
        true                        // allow_many
    );
}


void GUI::importMedia(const std::filesystem::path& filepath)
{
    const size_t count = _project.media().size();
    const Id id = _project.importMedia(filepath);

    // Into the selected folder.
    if (_project.media().size() != count && !_selectedFolder.empty()) {
        _project.moveMediaToFolder(id, _selectedFolder);
    }

    _selectedMedia = id;

    if (_project.media().size() != count) {
        commit("Import media");
    }
}


void GUI::commit(const std::string& label)
{
    _history.commit(_project, label);
    projectChanged();
}


void GUI::projectChanged()
{
    // A selected gap may no longer be one.
    _timeline.clearGap();

    _media.sync(_project);
    _media.applyProbeResults(_project);

    // Effects added: their plugins load now; their class is then known.
    _effects.sync(_project);
    _effects.completeSlots(_project);

    _mixDirty = true;
}


void GUI::updateMedia()
{
    const uint64_t revision = _media.revision();

    if (revision != _seenMediaRevision) {
        _seenMediaRevision = revision;
        _media.applyProbeResults(_project);
        _mixDirty = true;
    }

    matchSequenceToVideo();

    if (_mixDirty) {
        _mixDirty = false;
        _audio.setGraph(audio::buildMixGraph(_project,
            [this](Id mediaId, int stream) { return _media.audio(mediaId, stream); },
            [this](Id, const std::vector<EffectSlot>& slots) { return _effects.effectsOf(slots); }));
    }
}


void GUI::undo()
{
    endTextEditing();

    if (!_history.canUndo()) {
        return;
    }

    _history.undo(_project);
    projectChanged();

    // The selection may refer to clips the undo removed.
    std::erase_if(_selection, [&](Id id) { return _project.findClip(id) == nullptr; });
}


void GUI::redo()
{
    endTextEditing();

    if (!_history.canRedo()) {
        return;
    }

    _history.redo(_project);
    projectChanged();
    std::erase_if(_selection, [&](Id id) { return _project.findClip(id) == nullptr; });
}


void GUI::deleteSelection(bool ripple)
{
    if (_selection.empty()) {
        return;
    }

    if (ripple) {
        _project.rippleDelete(_selection, false);
    }
    else {
        _project.removeClips(_selection, false);
    }

    _selection.clear();
    commit(ripple ? "Ripple delete" : "Delete clips");
}


void GUI::splitAtPlayhead()
{
    const std::string before = _project.toJson({}).dump(0);

    _project.splitAt(_playhead, _selection);

    // Nothing to record when no clip crosses the playhead.
    if (_project.toJson({}).dump(0) != before) {
        commit("Split clips");
    }
}


void GUI::togglePlayback()
{
    if (_playing) {
        stopPlayback();
        return;
    }

    // Restart from the beginning when at the end.
    if (_playhead >= _project.sequence().duration()) {
        _playhead = 0;
    }

    _playing = true;
    _audio.play(_playhead);
}


void GUI::stopPlayback()
{
    // Only when playing: a scrub piece being heard goes on.
    if (_playing || _audio.playing()) {
        _playing = false;
        _audio.stop();
    }
}


void GUI::seek(Time t)
{
    stopPlayback();
    _playhead = std::max<Time>(0, snapToFrame(t, _project.sequence().frameRate));
}


void GUI::stepFrames(int64_t frames)
{
    const Rational& fps = _project.sequence().frameRate;
    seek(frameToTime(timeToFrame(_playhead, fps) + frames, fps));
    _audio.scrub(_playhead);
}


void GUI::updatePlayback()
{
    // Recording without playing the sequence: the playhead follows the
    // recording.
    if (_recorder.recording() && !_screenRecording && !_playing) {
        _playhead = _recordStart + mulDivFloor(_recorder.recordedFrames(), TicksPerSecond, audio::Recorder::SampleRate);
        return;
    }

    if (!_playing) {
        return;
    }

    // The audio clock drives the playhead.
    _playhead = _audio.position();

    const Time end = _project.sequence().duration();

    // Past the end while recording.
    if (_playhead >= end && !_recorder.recording()) {
        _playhead = end;
        stopPlayback();
    }
}


// ----------------------------------------------------------------------------
// GUIBase events
// ----------------------------------------------------------------------------

void GUI::eventFileDropped(const std::filesystem::path& filepath)
{
    if (filepath.extension() == std::string(".") + Project::FileExtension) {
        confirmThen(PendingAction::OpenProject, filepath);
    }
    else {
        importMedia(filepath);
    }
}


void GUI::eventOpenFile(SDL_Window* parent)
{
    SDL_ShowOpenFileDialog(
        GUI::onFileOpen,            // callback
        this,                       // userdata
        parent,                     // parent window
        projectFilters,
        SDL_arraysize(projectFilters),
        nullptr,                    // default location
        false                       // allow_many
    );
}


void GUI::eventSaveFile(SDL_Window* parent)
{
    // Ctrl + S in the shader editor: the shader.
    if (_shaderEditorFocused && !_actionAfterSave) {
        saveShader();
    }
    else if (_projectPath.empty()) {
        eventSaveAsFile(parent);
    }
    else if (saveProject(_projectPath) && std::exchange(_actionAfterSave, false)) {
        doPendingAction(std::exchange(_pendingAction, PendingAction::None), _pendingPath);
    }
}


void GUI::eventSaveAsFile(SDL_Window* parent)
{
    SDL_ShowSaveFileDialog(
        GUI::onFileSave,            // callback
        this,                       // userdata
        parent,                     // parent window
        projectFilters,
        SDL_arraysize(projectFilters),
        nullptr                     // default location
    );
}


void SDLCALL GUI::onFileOpen(void* userdata, const char* const* filelist, int filter)
{
    if (!userdata || !filelist || !filelist[0]) {
        return;
    }

    GUI* pGUI = static_cast<GUI*>(userdata);

    // SDL strings are UTF-8
    std::scoped_lock lock(pGUI->_dialogMutex);
    pGUI->_dialogOpenPath = std::filesystem::u8path(filelist[0]);
}


void SDLCALL GUI::onFileSave(void* userdata, const char* const* filelist, int filter)
{
    if (!userdata) {
        return;
    }

    GUI* pGUI = static_cast<GUI*>(userdata);

    // Cancelled: do not quit later on an unrelated save.
    if (!filelist || !filelist[0]) {
        std::scoped_lock lock(pGUI->_dialogMutex);
        pGUI->_dialogSavePath.clear();
        pGUI->_actionAfterSave = false;
        pGUI->_pendingAction = PendingAction::None;
        return;
    }

    // SDL strings are UTF-8
    std::filesystem::path filepath = std::filesystem::u8path(filelist[0]);

    if (filepath.extension().empty()) {
        filepath += std::string(".") + Project::FileExtension;
    }

    std::scoped_lock lock(pGUI->_dialogMutex);
    pGUI->_dialogSavePath = filepath;
}


void SDLCALL GUI::onFileImport(void* userdata, const char* const* filelist, int filter)
{
    if (!userdata || !filelist) {
        return;
    }

    GUI* pGUI = static_cast<GUI*>(userdata);
    std::scoped_lock lock(pGUI->_dialogMutex);

    for (const char* const* file = filelist; *file; file++) {
        pGUI->_dialogImportPaths.push_back(std::filesystem::u8path(*file));
    }
}


void GUI::eventBeforeFrame()
{
    if (_shaderReloadPending) {
        applyShaderReload();
    }

    // Test drag: about 4 s over the window, then dropped.
    if (_testDrag.active) {
        if (_testDragStart == 0) {
            _testDragStart = SDL_GetTicks();
        }

        _testDrag.dropped = SDL_GetTicks() - _testDragStart > 4000;
        setFileDrag(_testDrag);

        if (_testDrag.dropped) {
            _testDrag = FileDrag();
        }
    }

    // No mouse events while files are dragged over the window: the mouse is
    // where they are.
    if (fileDrag().active) {
        // In screen coordinates, as the windows can leave the main one.
        const ImVec2 origin = ImGui::GetMainViewport()->Pos;
        ImGui::GetIO().AddMousePosEvent(origin.x + fileDrag().x, origin.y + fileDrag().y);
    }

    // Decoders of a previous project: dropped between two frames, once the
    // GPU is done with their images.
    if (_resetDecoders) {
        _resetDecoders = false;
        vkDeviceWaitIdle(device().device());
        _decoders.clear();
    }

    // Actions asked from the menus, between two frames: their confirmation
    // popup opens on a frame of its own.
    if (_requestedAction != PendingAction::None) {
        confirmThen(std::exchange(_requestedAction, PendingAction::None));
    }

    std::filesystem::path openPath;
    std::filesystem::path savePath;
    std::vector<std::filesystem::path> importPaths;
    bool actionAfterSave = false;

    // Command line export: once the media are analyzed, then quit.
    if (_autoExport) {
        if (!_exporter && mediaInAnalysis() == 0 && _media.revision() == _seenMediaRevision && !startExport()) {
            SDL_Log("Export: nothing to export");
            GUIBase::quit();
        }

        if (_exporter && _exporter->done()) {
            SDL_Log("Export: %s%s", _exporter->succeeded() ? "done" : "failed: ", _exporter->error().c_str());
            _autoExport = false;
            GUIBase::quit();
        }
    }

    std::filesystem::path subtitlePath;

    {
        std::scoped_lock lock(_dialogMutex);

        if (!_dialogExportPath.empty()) {
            _exportPathText = pathToUtf8(_dialogExportPath);
            _dialogExportPath.clear();
        }

        subtitlePath.swap(_dialogSubtitlePath);

        openPath.swap(_dialogOpenPath);
        savePath.swap(_dialogSavePath);
        importPaths.swap(_dialogImportPaths);
        actionAfterSave = _actionAfterSave;
    }

    if (!subtitlePath.empty()) {
        if (_subtitleDialogSaves) {
            exportSubtitles(subtitlePath);
        }
        else {
            importSubtitles(subtitlePath);
        }
    }

    if (!openPath.empty()) {
        confirmThen(PendingAction::OpenProject, openPath);
    }

    if (!savePath.empty() && saveProject(savePath) && actionAfterSave) {
        _actionAfterSave = false;
        doPendingAction(std::exchange(_pendingAction, PendingAction::None), _pendingPath);
    }

    if (!importPaths.empty()) {
        const size_t count = _project.media().size();

        for (const std::filesystem::path& path : importPaths) {
            _selectedMedia = _project.importMedia(path);
        }

        if (_project.media().size() != count) {
            commit("Import media");
        }
    }
}

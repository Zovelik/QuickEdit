// Recording scenes: several sources recorded at once, each placed in the
// frame beforehand and recorded in a file of its own (see RecordingScene.h).
// Part of the GUI class.

#include <app/GUI.h>
#include <app/I18n.h>
#include <app/Widgets.h>

#include <fonts/IconsMaterialDesign.h>

#include <media/MediaFile.h>
#include <media/MediaManager.h>

#include <core/Paths.h>

#include <imgui_internal.h>
#include <imgui_stdlib.h>

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstring>


using namespace core;


namespace {

// Height over width of a source, in fractions of the frame: the rectangle of
// a source keeps the shape of its pictures.
float shapeOf(int frameWidth, int frameHeight, const Sequence& sequence)
{
    const float aspect = (frameWidth > 0 && frameHeight > 0) ? (float)frameHeight / (float)frameWidth : 9.f / 16.f;
    return aspect * (float)sequence.width / (float)std::max(1u, sequence.height);
}

} // namespace


GUI::ScenePreview::~ScenePreview()
{
    preview.stop();

    if (texture) {
        ImGui::UnregisterUserTexture(texture);
        texture->DestroyPixels();
        IM_DELETE(texture);
    }
}


RecordingScene* GUI::currentScene()
{
    return (_currentScene >= 0 && _currentScene < (int)_scenes.size()) ? &_scenes[(size_t)_currentScene] : nullptr;
}


void GUI::syncScenePreviews(bool show)
{
    RecordingScene* scene = currentScene();
    const size_t count = (show && scene) ? scene->sources.size() : 0;

    while (_scenePreviews.size() > count) {
        _scenePreviews.pop_back();
    }

    while (_scenePreviews.size() < count) {
        _scenePreviews.push_back(std::make_unique<ScenePreview>());
    }

    for (size_t i = 0; i < count; i++) {
        ScenePreview& preview = *_scenePreviews[i];
        const SceneSource& source = scene->sources[i];

        // While recording, the recorders have the sources.
        if (_sceneRecording || !source.available) {
            preview.preview.stop();
        }
        else {
            preview.preview.show(source.source);
        }

        if (!preview.texture) {
            preview.texture = IM_NEW(ImTextureData)();
            preview.texture->Create(ImTextureFormat_RGBA32, capture::SourcePreview::Width, capture::SourcePreview::Height);
            std::memset(preview.texture->GetPixels(), 0, (size_t)preview.texture->GetSizeInBytes());
            ImGui::RegisterUserTexture(preview.texture);
        }

        // The last picture: of the preview, or of the recording.
        bool fresh = false;

        if (_sceneRecording) {
            for (SceneTake& take : _sceneTakes) {
                if (take.sourceIndex == i && take.recorder) {
                    fresh = take.recorder->takePreview(preview.pixels);
                }
            }
        }
        else {
            fresh = preview.preview.take(preview.pixels);

            const auto [width, height] = preview.preview.frameSize();

            if (width > 0) {
                preview.frameWidth = width;
                preview.frameHeight = height;
            }
        }

        if (fresh && preview.texture->Status == ImTextureStatus_OK) {
            std::memcpy(preview.texture->GetPixels(), preview.pixels.data(),
                std::min(preview.pixels.size(), (size_t)preview.texture->GetSizeInBytes()));

            const ImTextureRect all = { 0, 0, (unsigned short)capture::SourcePreview::Width, (unsigned short)capture::SourcePreview::Height };
            preview.texture->UpdateRect = all;
            preview.texture->Updates.resize(0);
            preview.texture->Updates.push_back(all);
            preview.texture->SetStatus(ImTextureStatus_WantUpdates);
        }
    }
}


void GUI::sceneRecorderWindow()
{
    if (!_showSceneRecorder) {
        if (_sceneRecording) {
            stopSceneRecording();
        }

        syncScenePreviews(false);
        return;
    }

    if (!_captureSourcesListed) {
        _captureSources = capture::listSources();
        _captureSourcesListed = true;
    }

    if (!_scenesLoaded) {
        _scenes = loadScenes(_captureSources);
        _scenesLoaded = true;

        if (_scenes.empty()) {
            _scenes.push_back({ tr("Scene 1"), {} });
        }

        _currentScene = 0;
    }

    // The microphone, for its meter.
    if (_captureMicrophone && !_recorder.isOpen() && _recorderError.empty()) {
        _recorder.open(_inputDevice, _recorderError);
    }

    ImGui::SetNextWindowSize(ImVec2(40.f * ImGui::GetFontSize(), 44.f * ImGui::GetFontSize()), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));

    if (!ImGui::Begin(tr(ICON_MD_DASHBOARD " Record a Scene###sceneRecorder"), &_showSceneRecorder, ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoCollapse)) {
        syncScenePreviews(false);
        ImGui::End();
        return;
    }

    const bool recording = _sceneRecording;
    bool changed = false;

    // Scenes: choose, add, rename, remove.
    ImGui::BeginDisabled(recording);
    ImGui::PushStyleVarX(ImGuiStyleVar_ItemSpacing, ImGui::GetStyle().ItemInnerSpacing.x);

    RecordingScene* scene = currentScene();
    const float buttons = 3.f * (ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x);
    ImGui::SetNextItemWidth(-buttons);

    if (ImGui::BeginCombo("##scene", scene ? scene->name.c_str() : "")) {
        for (int i = 0; i < (int)_scenes.size(); i++) {
            ImGui::PushID(i);

            if (ImGui::Selectable(_scenes[(size_t)i].name.c_str(), i == _currentScene)) {
                _currentScene = i;
                _sceneSelected = -1;
                _scenePreviews.clear();
            }

            ImGui::PopID();
        }

        ImGui::EndCombo();
    }

    ImGui::SameLine();

    if (ImGui::Button(ICON_MD_ADD "##newScene", ImVec2(ImGui::GetFrameHeight(), 0.f))) {
        _scenes.push_back({ std::string(tr("Scene")) + " " + std::to_string(_scenes.size() + 1), {} });
        _currentScene = (int)_scenes.size() - 1;
        _sceneSelected = -1;
        _scenePreviews.clear();
        changed = true;
    }

    ImGui::SetItemTooltip("%s", tr("New scene"));
    ImGui::SameLine();

    if (ImGui::Button(ICON_MD_EDIT "##renameScene", ImVec2(ImGui::GetFrameHeight(), 0.f)) && scene) {
        _sceneName = scene->name;
        ImGui::OpenPopup("##renameScenePopup");
    }

    ImGui::SetItemTooltip("%s", tr("Rename the scene"));

    if (ImGui::BeginPopup("##renameScenePopup")) {
        if (ImGui::IsWindowAppearing()) {
            ImGui::SetKeyboardFocusHere();
        }

        if (ImGui::InputText("##name", &_sceneName, ImGuiInputTextFlags_EnterReturnsTrue) && scene && !_sceneName.empty()) {
            scene->name = _sceneName;
            changed = true;
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }

    ImGui::SameLine();

    if (ImGui::Button(ICON_MD_DELETE "##deleteScene", ImVec2(ImGui::GetFrameHeight(), 0.f)) && scene) {
        _scenes.erase(_scenes.begin() + _currentScene);

        if (_scenes.empty()) {
            _scenes.push_back({ tr("Scene 1"), {} });
        }

        _currentScene = 0;
        _sceneSelected = -1;
        _scenePreviews.clear();
        changed = true;
    }

    ImGui::SetItemTooltip("%s", tr("Delete the scene"));
    ImGui::PopStyleVar();
    ImGui::EndDisabled();

    scene = currentScene();

    if (!scene) {
        ImGui::End();
        return;
    }

    syncScenePreviews(true);

    // The frame of the sequence, the sources in it.
    const Sequence& sequence = _project.sequence();
    const float aspect = (float)sequence.height / (float)std::max(1u, sequence.width);
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float canvasWidth = std::min(avail.x, std::max(8.f, 0.55f * avail.y) / aspect);
    const ImVec2 canvasSize(canvasWidth, canvasWidth * aspect);
    const ImVec2 canvasMin = ImGui::GetCursorScreenPos();
    const ImVec2 canvasMax(canvasMin.x + canvasSize.x, canvasMin.y + canvasSize.y);

    ImGui::InvisibleButton("##canvas", canvasSize);
    const bool hovered = ImGui::IsItemHovered();
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    drawList->AddRectFilled(canvasMin, canvasMax, IM_COL32(0, 0, 0, 255));
    drawList->PushClipRect(canvasMin, canvasMax, true);

    auto rectOf = [&](const SceneSource& source) {
        return ImRect(
            ImVec2(canvasMin.x + source.x * canvasSize.x, canvasMin.y + source.y * canvasSize.y),
            ImVec2(canvasMin.x + (source.x + source.width) * canvasSize.x, canvasMin.y + (source.y + source.height) * canvasSize.y));
    };

    const float handle = 0.45f * ImGui::GetFontSize();

    for (size_t i = 0; i < scene->sources.size(); i++) {
        const SceneSource& source = scene->sources[i];
        const ImRect rect = rectOf(source);
        const ScenePreview* preview = (i < _scenePreviews.size()) ? _scenePreviews[i].get() : nullptr;

        if (preview && preview->texture && source.available) {
            // The picture without the letterboxing of the preview.
            const float fit = (preview->frameWidth > 0)
                ? std::min((float)capture::SourcePreview::Width / (float)preview->frameWidth, (float)capture::SourcePreview::Height / (float)preview->frameHeight)
                : 1.f;
            const float w = (preview->frameWidth > 0) ? (float)preview->frameWidth * fit / (float)capture::SourcePreview::Width : 1.f;
            const float h = (preview->frameHeight > 0) ? (float)preview->frameHeight * fit / (float)capture::SourcePreview::Height : 1.f;
            const ImVec2 uv0(0.5f - 0.5f * w, 0.5f - 0.5f * h);
            const ImVec2 uv1(0.5f + 0.5f * w, 0.5f + 0.5f * h);

            drawList->AddImage(preview->texture->GetTexRef(), rect.Min, rect.Max, uv0, uv1);
        }
        else {
            drawList->AddRectFilled(rect.Min, rect.Max, IM_COL32(60, 40, 40, 255));
            drawList->AddText(ImVec2(rect.Min.x + 4.f, rect.Min.y + 4.f), IM_COL32(230, 120, 110, 255), tr("Not found"));
        }

        const bool selected = (int)i == _sceneSelected;
        drawList->AddRect(rect.Min, rect.Max, selected ? IM_COL32(255, 122, 0, 255) : IM_COL32(255, 255, 255, 60), 0.f, 0, selected ? 2.f : 1.f);

        if (selected && !recording) {
            drawList->AddRectFilled(ImVec2(rect.Max.x - handle, rect.Max.y - handle), ImVec2(rect.Max.x + handle, rect.Max.y + handle), IM_COL32(255, 122, 0, 255));
        }
    }

    drawList->PopClipRect();

    // Mouse: select the top source under it, move it, or resize it by its
    // bottom right corner (its shape kept).
    const ImVec2 mouse = ImGui::GetIO().MousePos;

    if (!recording && hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        _sceneDrag = 0;

        if (_sceneSelected >= 0 && _sceneSelected < (int)scene->sources.size()) {
            const ImRect rect = rectOf(scene->sources[(size_t)_sceneSelected]);

            if (std::abs(mouse.x - rect.Max.x) < 1.5f * handle && std::abs(mouse.y - rect.Max.y) < 1.5f * handle) {
                _sceneDrag = 2;
            }
        }

        if (_sceneDrag == 0) {
            _sceneSelected = -1;

            for (int i = (int)scene->sources.size() - 1; i >= 0; i--) {
                if (rectOf(scene->sources[(size_t)i]).Contains(mouse)) {
                    _sceneSelected = i;
                    _sceneDrag = 1;
                    break;
                }
            }
        }

        if (_sceneDrag != 0) {
            _sceneDragStart = mouse;
            _sceneDragOriginal = scene->sources[(size_t)_sceneSelected];
        }
    }

    if (_sceneDrag != 0 && _sceneSelected >= 0 && _sceneSelected < (int)scene->sources.size()) {
        SceneSource& source = scene->sources[(size_t)_sceneSelected];
        const float dx = (mouse.x - _sceneDragStart.x) / canvasSize.x;
        const float dy = (mouse.y - _sceneDragStart.y) / canvasSize.y;

        if (_sceneDrag == 1) {
            source.x = _sceneDragOriginal.x + dx;
            source.y = _sceneDragOriginal.y + dy;
        }
        else {
            const ScenePreview* preview = _scenePreviews[(size_t)_sceneSelected].get();
            const float shape = shapeOf(preview->frameWidth, preview->frameHeight, sequence);
            source.width = std::max(0.03f, _sceneDragOriginal.width + dx);
            source.height = source.width * shape;
        }

        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            _sceneDrag = 0;
            changed = true;
        }
    }

    if (hovered && _sceneDrag == 0) {
        ImGui::SetItemTooltip("%s", tr("Drag a source to move it, its corner to resize it."));
    }

    // The sources, the top one first.
    ImGui::BeginDisabled(recording);
    ImGui::PushStyleVarX(ImGuiStyleVar_ItemSpacing, ImGui::GetStyle().ItemInnerSpacing.x);

    if (ImGui::BeginCombo("##addSource", tr(ICON_MD_ADD " Add a Source..."), ImGuiComboFlags_HeightLarge)) {
        const char* titles[] = { tr("Screens"), tr("Windows"), tr("Cameras") };
        int group = -1;

        for (const capture::CaptureSource& available : _captureSources) {
            if ((int)available.kind != group) {
                group = (int)available.kind;
                ImGui::SeparatorText(titles[group]);
            }

            ImGui::PushID((const void*)(uintptr_t)available.handle);
            ImGui::PushID(available.device.c_str());

            if (ImGui::Selectable(available.name.c_str())) {
                // A quarter of the frame, cascading, until its shape is known.
                SceneSource source;
                source.source = available;
                source.width = scene->sources.empty() ? 1.f : 0.4f;
                source.height = source.width * shapeOf(0, 0, sequence);
                source.x = scene->sources.empty() ? 0.f : 0.05f * (float)(scene->sources.size() % 8);
                source.y = scene->sources.empty() ? 0.f : 0.05f * (float)(scene->sources.size() % 8);
                scene->sources.push_back(source);
                _sceneSelected = (int)scene->sources.size() - 1;
                changed = true;
            }

            ImGui::PopID();
            ImGui::PopID();
        }

        ImGui::EndCombo();
    }

    ImGui::SameLine();

    if (ImGui::Button(ICON_MD_REFRESH "##sceneSources", ImVec2(ImGui::GetFrameHeight(), 0.f))) {
        _captureSourcesListed = false;
    }

    ImGui::SetItemTooltip("%s", tr("Look for screens and windows again"));
    ImGui::PopStyleVar();

    for (int i = (int)scene->sources.size() - 1; i >= 0; i--) {
        const SceneSource& source = scene->sources[(size_t)i];
        const char* icon = source.source.kind == capture::CaptureSource::Kind::Monitor ? ICON_MD_MONITOR
            : source.source.kind == capture::CaptureSource::Kind::Camera ? ICON_MD_VIDEOCAM : ICON_MD_WEB_ASSET;
        const std::string label = std::string(icon) + " " + source.source.name + (source.available ? "" : std::string("  (") + tr("Not found") + ")");

        ImGui::PushID(i);

        if (ImGui::Selectable(label.c_str(), i == _sceneSelected)) {
            _sceneSelected = i;
        }

        ImGui::PopID();
    }

    // The selected source: order, size, removal.
    if (_sceneSelected >= 0 && _sceneSelected < (int)scene->sources.size()) {
        const size_t index = (size_t)_sceneSelected;
        SceneSource& source = scene->sources[index];
        const ScenePreview* preview = (index < _scenePreviews.size()) ? _scenePreviews[index].get() : nullptr;
        const int frameWidth = preview ? preview->frameWidth : 0;
        const int frameHeight = preview ? preview->frameHeight : 0;
        const float shape = shapeOf(frameWidth, frameHeight, sequence);

        ImGui::PushStyleVarX(ImGuiStyleVar_ItemSpacing, ImGui::GetStyle().ItemInnerSpacing.x);

        if (widgets::panelButton(ICON_MD_ARROW_UPWARD, tr("Above the next source"), false, index + 1 < scene->sources.size())) {
            std::swap(scene->sources[index], scene->sources[index + 1]);
            std::swap(_scenePreviews[index], _scenePreviews[index + 1]);
            _sceneSelected++;
            changed = true;
        }

        if (widgets::panelButton(ICON_MD_ARROW_DOWNWARD, tr("Below the previous source"), false, index > 0)) {
            std::swap(scene->sources[index], scene->sources[index - 1]);
            std::swap(_scenePreviews[index], _scenePreviews[index - 1]);
            _sceneSelected--;
            changed = true;
        }

        if (widgets::panelButton(ICON_MD_FIT_SCREEN, tr("Fit the frame"))) {
            source.width = std::min(1.f, 1.f / shape);
            source.height = source.width * shape;
            source.x = 0.5f * (1.f - source.width);
            source.y = 0.5f * (1.f - source.height);
            changed = true;
        }

        if (widgets::panelButton(ICON_MD_PHOTO_SIZE_SELECT_ACTUAL, tr("Native size: one pixel of the source per pixel of the sequence"), false, frameWidth > 0)) {
            source.width = (float)frameWidth / (float)std::max(1u, sequence.width);
            source.height = source.width * shape;
            changed = true;
        }

        if (widgets::panelButton(ICON_MD_DELETE, tr("Remove the source"))) {
            scene->sources.erase(scene->sources.begin() + _sceneSelected);
            _scenePreviews.erase(_scenePreviews.begin() + _sceneSelected);
            _sceneSelected = -1;
            changed = true;
        }

        ImGui::NewLine();
        ImGui::PopStyleVar();
    }

    // Options, shared with the screen recorder.
    ImGui::Checkbox(tr("Mouse cursor"), &_captureCursor);
    ImGui::SameLine();
    ImGui::Checkbox(tr("Hide QuickEdit from the recording"), &_hideFromCapture);
    ImGui::Checkbox(tr("Computer sound"), &_captureSound);
    ImGui::SameLine();
    ImGui::Checkbox(tr("Microphone"), &_captureMicrophone);
    ImGui::SetItemTooltip(tr("Recorded on an audio track of its own."));
    ImGui::EndDisabled();

    if (_captureMicrophone) {
        inputDeviceCombo(!recording);
        levelMeter("##sceneMicrophone", _recorder.takePeak(), _microphoneMeter);
    }

    ImGui::Spacing();

    // Record / Stop
    const float width = 8.f * ImGui::GetFontSize();
    const bool busy = _screenRecording || (_recorder.recording() && !_sceneRecording);
    const bool any = std::any_of(scene->sources.begin(), scene->sources.end(), [](const SceneSource& s) { return s.available; });

    if (!recording) {
        ImGui::BeginDisabled(busy || !any);
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.75f, 0.18f, 0.16f, 1.f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.85f, 0.25f, 0.22f, 1.f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.65f, 0.14f, 0.12f, 1.f));

        if (ImGui::Button(tr(ICON_MD_FIBER_MANUAL_RECORD " Record"), ImVec2(width, 0.f))) {
            startSceneRecording();
        }

        ImGui::PopStyleColor(3);
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled(tr("At the playhead, %s"), formatTimecode(_playhead, sequence.frameRate).c_str());
    }
    else {
        if (ImGui::Button(tr(ICON_MD_STOP " Stop"), ImVec2(width, 0.f))) {
            stopSceneRecording();
        }

        double seconds = 0.0;

        for (const SceneTake& take : _sceneTakes) {
            seconds = std::max(seconds, take.recorder->seconds());
        }

        const bool on = std::fmod(ImGui::GetTime(), 1.0) < 0.6;
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(ImVec4(0.95f, 0.3f, 0.25f, on ? 1.f : 0.25f), ICON_MD_FIBER_MANUAL_RECORD);
        ImGui::SameLine();
        ImGui::Text(tr("Recording  %s"), formatTimecode(secondsToTime(seconds), sequence.frameRate).c_str());
    }

    if (!_sceneError.empty()) {
        ImGui::PushTextWrapPos(0.f);
        ImGui::TextColored(ImVec4(0.9f, 0.35f, 0.3f, 1.f), ICON_MD_WARNING " %s", _sceneError.c_str());
        ImGui::PopTextWrapPos();
    }

    ImGui::TextDisabled("%s", tr("Each source is recorded in a file of its own, and placed on a video track of its own."));

    if (changed) {
        saveScenes(_scenes);
    }

    ImGui::End();
}


void GUI::startSceneRecording()
{
    RecordingScene* scene = currentScene();

    if (!scene || _sceneRecording || _screenRecording) {
        return;
    }

    stopPlayback();
    _sceneError.clear();
    _sceneTakes.clear();

    if (_hideFromCapture) {
        capture::excludeFromCapture(true);
    }

    // The sources let go by their previews (a camera opens once).
    for (const std::unique_ptr<ScenePreview>& preview : _scenePreviews) {
        preview->preview.stop(true);
    }

    const Sequence& sequence = _project.sequence();
    bool first = true;

    for (size_t i = 0; i < scene->sources.size(); i++) {
        const SceneSource& source = scene->sources[i];

        if (!source.available) {
            continue;
        }

        capture::ScreenRecordSettings settings;
        settings.source = source.source;
        settings.frameRateNum = (int)sequence.frameRate.num;
        settings.frameRateDen = (int)sequence.frameRate.den;
        settings.cursor = _captureCursor;
        settings.systemAudio = first && _captureSound;

        const std::string prefix = scene->name + " - " + std::to_string(i + 1);
        const std::filesystem::path path = recordingPath(prefix.c_str(), ".mkv");
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);

        SceneTake take;
        take.recorder = std::make_unique<capture::ScreenRecorder>();
        take.path = path;
        take.sourceIndex = i;
        take.source = source;

        std::string error;

        if (!take.recorder->start(path, settings, error)) {
            _sceneError = source.source.name + ": " + error;
            break;
        }

        _sceneTakes.push_back(std::move(take));
        first = false;
    }

    // A source that could not start: nothing is recorded.
    if (!_sceneError.empty() || _sceneTakes.empty()) {
        for (SceneTake& take : _sceneTakes) {
            take.recorder->stop();
            std::error_code ec;
            std::filesystem::remove(take.path, ec);
        }

        _sceneTakes.clear();
        capture::excludeFromCapture(false);
        return;
    }

    _sceneStart = snapToFrame(_playhead, sequence.frameRate);
    _sceneRecording = true;
    _sceneMicrophonePath.clear();

    // The microphone, added to the file of the first source at the end.
    if (_captureMicrophone) {
        if (!_recorder.isOpen()) {
            _recorder.open(_inputDevice, _recorderError);
        }

        const std::filesystem::path microphonePath = recordingPath((scene->name + " - voice").c_str(), ".wav");

        if (_recorder.isOpen() && _recorder.start(microphonePath, _recorderError)) {
            _sceneMicrophonePath = microphonePath;
        }
    }
}


void GUI::stopSceneRecording()
{
    if (!_sceneRecording) {
        return;
    }

    _sceneRecording = false;

    double seconds = 0.0;
    std::vector<SceneTake> takes = std::move(_sceneTakes);
    _sceneTakes.clear();

    for (SceneTake& take : takes) {
        seconds = std::max(seconds, take.recorder->seconds());

        if (!take.recorder->stop()) {
            _sceneError = take.source.source.name + ": " + take.recorder->error();
        }
    }

    const int64_t microphoneFrames = _sceneMicrophonePath.empty() ? 0 : _recorder.stop();
    capture::excludeFromCapture(false);

    std::error_code ec;

    if (seconds <= 0.0 || takes.empty()) {
        for (SceneTake& take : takes) std::filesystem::remove(take.path, ec);
        if (!_sceneMicrophonePath.empty()) std::filesystem::remove(_sceneMicrophonePath, ec);
        return;
    }

    // The microphone in the file of the first source.
    if (microphoneFrames > 0) {
        std::string error;

        if (capture::addAudioStream(takes.front().path, _sceneMicrophonePath, "Microphone", error)) {
            std::filesystem::remove(_sceneMicrophonePath, ec);
        }
        else {
            SDL_Log("Scene recording: the microphone stays apart in %s: %s", _sceneMicrophonePath.string().c_str(), error.c_str());
        }
    }

    // A video track per source, free from the start (with its sound track):
    // the lower ones for the first sources, new ones on top when needed.
    Sequence& sequence = _project.sequence();
    const Time start = _sceneStart;

    auto freeFrom = [&](const Track& track) {
        return !track.locked && std::none_of(track.clips.begin(), track.clips.end(), [&](const Clip& clip) { return clip.end() > start; });
    };

    std::vector<Id> tracks;

    for (size_t i = 0; i < sequence.videoTracks.size() && tracks.size() < takes.size(); i++) {
        if (freeFrom(sequence.videoTracks[i]) && (i >= sequence.soundTracks.size() || freeFrom(sequence.soundTracks[i]))) {
            tracks.push_back(sequence.videoTracks[i].id);
        }
    }

    while (tracks.size() < takes.size()) {
        const Id track = _project.newId();
        sequence.addTrack(TrackType::Video, track, _project.newId());
        tracks.push_back(track);
    }

    _selection.clear();
    Time end = start;

    for (size_t i = 0; i < takes.size(); i++) {
        const SceneTake& take = takes[i];
        const Id mediaId = _project.importMedia(take.path);

        if (MediaItem* item = _project.findMedia(mediaId)) {
            try {
                media::MediaManager::applyProbe(*item, media::probe(take.path));
            }
            catch (const std::exception& e) {
                SDL_Log("Scene recording: %s", e.what());
            }
        }

        const Id clipId = _project.addClip(mediaId, tracks[i], start);
        Clip* clip = _project.findClip(clipId);
        const MediaItem* item = _project.findMedia(mediaId);

        if (!clip || !item) {
            continue;
        }

        // In its rectangle: the picture fits the frame, scaled and moved.
        const float frameWidth = (float)sequence.width;
        const float frameHeight = (float)sequence.height;
        const float fit = fitScale(FitMode::Fit, frameWidth, frameHeight, (float)item->width, (float)item->height);
        const float fittedWidth = std::max(1.f, (float)item->width * fit);

        clip->fit = FitMode::Fit;
        clip->transform.scale = take.source.width * frameWidth / fittedWidth;
        clip->transform.x = (take.source.x + 0.5f * take.source.width) * frameWidth - 0.5f * frameWidth;
        clip->transform.y = (take.source.y + 0.5f * take.source.height) * frameHeight - 0.5f * frameHeight;

        end = std::max(end, clip->end());
        TimelineView::addWithLinks(_project, clipId, _selection);
    }

    _playhead = end;
    commit("Record scene");
}

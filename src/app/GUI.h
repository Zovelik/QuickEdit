#pragma once

#include <app/EffectRack.h>
#include <audio/Vst3Plugin.h>
#include <app/Exporter.h>
#include <app/GUIBase.h>
#include <app/Inspector.h>
#include <app/SubtitleGenerator.h>
#include <app/KeyframeEditor.h>
#include <app/TextEditor.h>
#include <app/TimelineView.h>

#include <audio/AudioEngine.h>
#include <audio/Recorder.h>
#include <capture/ScreenRecorder.h>
#include <core/History.h>
#include <core/Project.h>
#include <core/ThreadPool.h>
#include <media/HwDevice.h>
#include <media/MediaManager.h>
#include <media/Thumbnails.h>
#include <media/VideoPlayer.h>
#include <render/Compositor.h>
#include <render/FontLibrary.h>
#include <render/VideoFrameRenderer.h>

#include <atomic>
#include <filesystem>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>


class GUI : public GUIBase
{
public:
    // Opens the given project, if any.
    explicit GUI(const std::filesystem::path& projectPath = {}, double startSeconds = 0.0, core::Id selectedClip = core::InvalidId);

    // Exports the project to path as soon as its media are analyzed, then
    // quits (command line --export).
    void exportAndQuit(const std::filesystem::path& path, ExportSettings::Codec codec, uint32_t height = 0, int frameRate = 0);

    // Edits the selected text clip in the viewer (command line --edit).
    void editSelectedText() { if (!_selection.empty()) beginTextEditing(_selection.front()); }

    // Opens the editor of the plugin of an effect slot (command line
    // --effect-editor).
    void openEffectEditor(core::Id slotId);

    // Drags a file over the window at (x, y) for a few seconds, then drops
    // it (command line --drop, for tests).
    void testFileDrop(const std::filesystem::path& path, float x, float y);

    // Records the default input for some seconds (command line --record,
    // for tests).
    void testRecord(double seconds) { _showRecorder = true; _testRecordSeconds = seconds; }

    // Shows the preview of a source (index in the list) for 3 s, then
    // records it for some seconds, QuickEdit visible in the capture
    // (command line --record-screen, for tests).
    void testRecordScreen(double seconds, int source)
    {
        _showScreenRecorder = true;
        _hideFromCapture = false;
        _testScreenSeconds = seconds;
        _testScreenSource = source;
        _testScreenStartTicks = SDL_GetTicks() + 3000;
    }

    // Quits after some seconds, without saving (command line --quit-after,
    // for tests: never kill the application).
    // Plays from the start time (command line --play, for tests).
    void testPlay() { togglePlayback(); }

    // Opens the Project Properties window (command line --properties).
    void openProjectProperties() { _requestProjectProperties = true; }

    // Shows the Effects tab of the inspector (command line --effects).
    void showEffects() { _showEffectsTab = true; }

    // Shows the Keyframes panel (command line --keyframes).
    void showKeyframes() { _focusKeyframes = true; }
    void testEditShader(const std::string& name) { _testEditShader = name; }
    bool _focusKeyframes = false;

    void testQuitAfter(double seconds) { _testQuitTicks = SDL_GetTicks() + (uint64_t)(seconds * 1000.0); }

    ~GUI();

    virtual void render(
        VkCommandBuffer commandBuffer,
        VkImage targetImage,
        VkImageView targetImageView,
        VkImageLayout targetLayout
    ) override;

    // Asks to save the changes first when the project is modified.
    virtual void quit() override;

private:
    void beginMainWindow();
    void endMainWindow();

    // Renders the menus. In a borderless window the menu bar is also the
    // title bar: it starts with the application icon and ends with the
    // window buttons.
    void menuBar();

    // Title bar parts, drawn inside the menu bar (borderless window only).
    void titlebarIcon();
    void titlebarButtons();

    // Frame padding giving the menu bar the height of the title bar, for the
    // current display scale. It must be the one in use when the Main window
    // begins and while its menu bar is built.
    ImVec2 titlebarFramePadding() const;

    // Renders the toolbar (file, edit and playback commands) below the menu
    // bar.
    void toolbar();

    // Handles the keyboard shortcuts of the edit and playback commands. File
    // shortcuts are handled by GUIBase.
    void shortcuts();

    // Builds the default docking layout on first launch.
    void dockLayout(ImGuiID dockSpaceId);

    // Dockable panels
    void mediaPanel();
    void viewerPanel();
    void inspectorPanel();
    void timelinePanel();

    // Tabs of the inspector: the selected clip, the effects of its track.
    void clipTab();
    void effectsTab();

    // A chain of VST effects (of a track or a clip): bypass, editor, order,
    // the plugins to add.
    void audioEffectChain(std::vector<core::EffectSlot>& slots, const char* id);
    bool _showEffectsTab = false;

    // Media panel focused, for its Delete key.
    bool _mediaPanelFocused = false;

    // Removes a media and its clips, confirmed first when it has clips.
    std::vector<core::Id> clipsOfMedia(core::Id mediaId) const;
    void removeMediaAndClips(const std::vector<core::Id>& mediaIds);
    std::vector<core::Id> _confirmRemoveMedia;

    // Popups
    void exportPopup();
    void errorPopup();
    void unsavedChangesPopup();

    // Reports an error to the user. Safe to call from any thread.
    void setErrorMessage(const std::string& message);

    // Window title: project name and modified state.
    void updateWindowTitle();

    // ------------------------------------------------------------------------
    // Commands
    // ------------------------------------------------------------------------

    // What to do once the changes are saved or discarded, see
    // confirmThen().
    enum class PendingAction
    {
        None,
        Quit,
        NewProject,
        OpenProject
    };

    // Does the action, after asking to save the changes when the project
    // is modified.
    void confirmThen(PendingAction action, const std::filesystem::path& path = {});
    void doPendingAction(PendingAction action, const std::filesystem::path& path);

    void newProject();

    // Replaces the project (new, opened), forgetting what was analyzed,
    // decoded and selected for the previous one.
    void replaceProject(core::Project project, const std::filesystem::path& path);
    bool _resetDecoders = false;

    // A layer of the viewer waits for its media (analysis, decoding).
    bool _viewerLoading = false;
    uint64_t _viewerLoadingSince = 0;     // Ticks; 0 while showing
    void openProject(const std::filesystem::path& filepath);
    bool saveProject(const std::filesystem::path& filepath);
    void requestImportMedia();
    void importMedia(const std::filesystem::path& filepath);

    // Records the current state of the project in the undo history.
    void commit(const std::string& label);

    // To call whenever the project was replaced or edited: starts the
    // analysis of new media and rebuilds the audio mix.
    void projectChanged();

    // Applies the media analyzed in the background, updates the audio mix.
    void updateMedia();

    // A video analyzed for the first time, larger or of a higher frame rate
    // than the sequence, while no other picture is on it: the sequence takes
    // its size and rate. The media of an opened project are not considered.
    void matchSequenceToVideo();
    std::set<core::Id> _matchedMedia;
    bool _matchedMediaSeeded = false;

    void undo();
    void redo();
    void deleteSelection(bool ripple);
    void splitAtPlayhead();

    void togglePlayback();
    void stopPlayback();
    void seek(core::Time t);
    void stepFrames(int64_t frames);

    // Advances the playhead while playing.
    void updatePlayback();

    // Creates the FFmpeg Vulkan device, the fonts and the compositor.
    void createVideo();

    // Requests the frames of the visible clips at the playhead, one decoder
    // per video track, and records the composition of the frame.
    void cmdCompose(VkCommandBuffer commandBuffer);

    // Moves the layers with the mouse in the viewer, over the frame of the
    // sequence shown in [frameMin, frameMin + frameSize].
    void viewerInteraction(const ImVec2& frameMin, const ImVec2& frameSize);

    // Starts exporting with _exportSettings. False when the media are not all
    // analyzed yet.
    bool startExport();

    // Media still being analyzed, which an export has to wait for.
    int mediaInAnalysis() const;

    void addText();

    // Subtitles: a line added at the playhead, the lines of a SubRip file
    // imported into a new track, those shown exported to one.
    void addSubtitle();

    // Subtitles from the speech of the sequence: the model chosen (and
    // downloaded) for the computer, the language, GPU or not.
    void subtitleGeneratorWindow();
    std::vector<float> speechSamples(std::string& error);
    core::Id _subtitleSourceTrack = core::InvalidId;     // None: the whole sequence
    bool _showSubtitleGenerator = false;
    ModelDownload _modelDownload;
    SubtitleGenerator _subtitleGenerator;
    std::string _whisperModel;
    int _whisperLanguage = 0;
    bool _whisperGpu = true;
    std::string _generatorError;
    bool _testTranscribe = false;

public:
    // Generates the subtitles with a model, logs them, quits (command line
    // --transcribe, for tests).
    void testTranscribe(const std::string& model) { _showSubtitleGenerator = true; _whisperModel = model; _testTranscribe = true; }

private:
    void importSubtitles(const std::filesystem::path& path);
    bool exportSubtitles(const std::filesystem::path& path);
    static void SDLCALL onSubtitleFile(void* userdata, const char* const* filelist, int filter);
    std::filesystem::path _dialogSubtitlePath;
    bool _subtitleDialogSaves = false;
    bool _exportSubtitles = true;

    // Sequences: shown in the timeline (one tab each), added, renamed,
    // removed. The playhead of each is kept.
    void sequenceTabs();
    void showSequence(core::Id id);
    std::map<core::Id, core::Time> _sequencePlayheads;
    core::Id _selectTab = core::InvalidId;
    core::Id _renamingSequence = core::InvalidId;
    std::string _sequenceName;

    // Shaders of the user (Documents/QuickEdit/Shaders), generators added
    // at the playhead.
    std::vector<core::ShaderDef> _userShaders;
    std::filesystem::path shaderFolder() const;
    void reloadShaders(bool commitChanges);
    void openShaderFolder();
    void addGenerator(const core::ShaderDef& shader);

    // Shader editor: a .glsl file of the shaders folder, saved (Ctrl + S)
    // and used at once, with the messages of the compiler as it is typed.
    bool _showShaderEditor = false;
    bool _shaderEditorFocused = false;
    std::filesystem::path _shaderEditorPath;
    std::string _shaderEditorText;
    std::string _shaderEditorSaved;
    std::string _shaderEditorError;
    double _shaderEditorChecked = 0.;       // Last typed, or compiled
    bool _shaderEditorDirty = false;        // Typed since
    const core::ShaderDef* newShader(core::ShaderKind kind);
    void editShader(const std::string& shaderId);
    void saveShader();
    void checkEditedShader();
    void shaderEditorWindow();
    std::string _testEditShader;

    // Closes the gap selected in the timeline.
    void closeGap();

    // Files dragged from the file manager: probed in the background for
    // their lengths, previewed on the timeline, and placed there or
    // imported once dropped.
    struct DropProbe
    {
        std::atomic<bool> done = false;
        bool ok = false;
        media::ProbeResult result;
    };

    void fileDragPreview();
    void fileDropped();
    std::map<std::filesystem::path, std::shared_ptr<DropProbe>> _dropProbes;
    core::Id _dropTrack = core::InvalidId;
    core::Time _dropStart = 0;
    FileDrag _testDrag;
    uint64_t _testDragStart = 0;

    // Dissolve into the selected video clips (or the top one at the
    // playhead), or removes it when they all have one.
    void toggleTransition();
    void freezeFrame();
    void addTrack(core::TrackType type);

    // ------------------------------------------------------------------------
    // GUIBase events
    // ------------------------------------------------------------------------

    void eventFileDropped(const std::filesystem::path& filepath);
    virtual void eventOpenFile(SDL_Window* parent) override;
    virtual void eventSaveFile(SDL_Window* parent) override;
    virtual void eventSaveAsFile(SDL_Window* parent) override;
    virtual void eventBeforeFrame() override;
    virtual void eventSdl(const SDL_Event& event) override;

    static void SDLCALL onFileOpen(void* userdata, const char* const* filelist, int filter);
    static void SDLCALL onFileSave(void* userdata, const char* const* filelist, int filter);
    static void SDLCALL onFileImport(void* userdata, const char* const* filelist, int filter);
    static void SDLCALL onExportFile(void* userdata, const char* const* filelist, int filter);

    // ------------------------------------------------------------------------
    // State
    // ------------------------------------------------------------------------

    // Background tasks. Declared first: destroyed last.
    core::ThreadPool _threadPool;
    media::MediaManager _media{ _threadPool };

    // Pictures of the videos, for the timeline and the media panel: the
    // strips of the files, and their ImGui textures (kept for the session,
    // freed by the ImGui backend at its shutdown).
    media::ThumbnailCache _thumbnails{ _threadPool };

    struct ThumbnailTexture
    {
        std::shared_ptr<media::ThumbnailStrip> strip;
        ImTextureData* texture = nullptr;
    };

    std::map<std::filesystem::path, ThumbnailTexture> _thumbnailTextures;
    TimelineView::Thumbnails thumbnailsOf(core::Id mediaId);
    audio::AudioEngine _audio;

    // Voice-over recording, in a window of its own.
    void recorderWindow();
    // Peak meter of an input, the level falling back slowly.
    struct LevelMeter
    {
        float levelDb = -100.f;
        float peakDb = -100.f;
        double peakTime = 0.0;
        double clipTime = -100.0;
    };

    void levelMeter(const char* id, float peak, LevelMeter& meter);

    // Folder of the recordings (next to the project, or in the documents),
    // and a new file name there.
    std::filesystem::path recordingPath(const char* prefix, const char* extension) const;
    void startRecording();
    void stopRecording();
    audio::Recorder _recorder;
    bool _showRecorder = false;
    std::vector<audio::InputDevice> _inputDevices;
    bool _inputDevicesListed = false;
    SDL_AudioDeviceID _inputDevice = 0;
    std::string _recorderError;
    bool _playWhileRecording = true;
    core::Time _recordStart = 0;

    // Track of the recording, chosen at its start: free from there on
    // (_recordTrackAdded: added for it).
    core::Id _recordTrack = core::InvalidId;
    bool _recordTrackAdded = false;

    // Waveform of the recording being made, coarser levels built as it
    // grows.
    std::shared_ptr<media::Waveform> _liveWaveform;
    void updateLiveWaveform();
    std::filesystem::path _recordPath;
    LevelMeter _inputMeter;

    // Screen recording, in a window of its own; the microphone, through
    // _recorder, recorded beside it on request.
    void screenRecorderWindow();
    void startScreenRecording();
    void stopScreenRecording();
    capture::ScreenRecorder _screenRecorder;
    bool _showScreenRecorder = false;
    std::vector<capture::CaptureSource> _captureSources;
    bool _captureSourcesListed = false;
    capture::CaptureSource _captureSource;
    int _captureFrameRate = 0;     // 0: the one of the sequence
    bool _captureCursor = true;
    bool _captureSound = true;
    bool _captureMicrophone = false;
    bool _hideFromCapture = true;
    bool _screenRecording = false;
    std::string _screenError;
    core::Time _screenStart = 0;
    std::filesystem::path _screenPath;
    std::filesystem::path _screenMicrophonePath;

    // Tracks of the recording, chosen at its start (added for it, if so).
    core::Id _screenTrack = core::InvalidId;
    core::Id _screenMicrophoneTrack = core::InvalidId;
    bool _screenTrackAdded = false;
    bool _screenMicrophoneTrackAdded = false;

    // Live picture of the chosen source, in an ImGui texture (never freed:
    // the ImGui backend releases it at its shutdown).
    capture::SourcePreview _sourcePreview;
    ImTextureData* _previewTexture = nullptr;
    std::vector<uint8_t> _previewPixels;
    void sourcePreview();
    LevelMeter _systemMeter;
    LevelMeter _microphoneMeter;
    double _testScreenSeconds = 0.0;
    int _testScreenSource = 0;
    uint64_t _testScreenStartTicks = 0;
    double _testRecordSeconds = 0.0;
    uint64_t _testQuitTicks = 0;

    // VST3 effects of the audio tracks, see effectsWindow().
    EffectRack _effects;
    core::Id _effectsTrack = core::InvalidId;
    std::vector<audio::PluginFile> _pluginFiles;
    bool _pluginFilesListed = false;

    // Video decoding and display, see createVideo(). Declared in this order:
    // the frames reference the device.
    std::unique_ptr<media::HwDevice> _hwDevice;
    std::unique_ptr<FontLibrary> _fonts;
    std::unique_ptr<Compositor> _compositor;
    // Decoders of the viewer, by layer key (track, slot and nesting, see
    // core::LayerRequest), so that the layers decode in parallel.
    struct Decoder
    {
        std::unique_ptr<media::VideoPlayer> player;
        std::unique_ptr<VideoFrameRenderer> renderer;
        bool used = false;
        int idleFrames = 0;
    };

    std::map<uint64_t, Decoder> _decoders;

    // The subtitles of the frame being composited, as text clips.
    std::vector<core::Clip> _subtitleClips;
    std::set<core::Id> _loggedVideoMedia;

    Inspector _inspector;

    // Curves of the animations of the selected clip, in a tab beside the
    // timeline.
    void keyframesPanel();
    KeyframeEditor _keyframes;
    TextEditor _textEditor;

    // Starts / stops editing a text in the viewer.
    void beginTextEditing(core::Id clipId);
    void endTextEditing();

    // Export
    std::vector<uint8_t> _defaultFontData;
    std::unique_ptr<Exporter> _exporter;
    ExportSettings _exportSettings;
    std::string _exportPathText;
    bool _requestExportPopup = false;

    // Settings of the sequence, from the Edit menu.
    void projectPropertiesPopup();
    bool _requestProjectProperties = false;
    bool _autoExport = false;

    // Layer moved, scaled or rotated in the viewer: its placement when the
    // drag started.
    enum class LayerDrag
    {
        Move,
        Scale,          // Corner handles
        Rotate,         // Handle above the top edge
        Crop            // Edge handles, or Alt and a corner (as in OBS)
    };

    // Sides cropped by the handle dragged.
    bool _cropLeft = false;
    bool _cropRight = false;
    bool _cropTop = false;
    bool _cropBottom = false;

    core::Id _draggedLayer = core::InvalidId;
    LayerDrag _layerDrag = LayerDrag::Move;
    ImVec2 _dragStartMouse;
    ImVec2 _dragCenter;             // Center of the layer, sequence pixels
    float _dragStartX = 0.f;
    float _dragStartY = 0.f;
    float _dragStartScale = 1.f;
    float _dragStartRotation = 0.f;
    bool _dragMoved = false;

    core::Project _project;
    core::History _history;
    std::filesystem::path _projectPath;

    std::vector<core::Id> _selection;
    core::Id _selectedMedia = core::InvalidId;

    // Media selected together (Ctrl + click, or a rectangle dragged from the
    // empty part of the list): _selectedMedia is the last one clicked.
    std::vector<core::Id> _mediaSelection;
    bool _mediaMarquee = false;
    ImVec2 _mediaMarqueeStart;
    std::vector<core::Id> _mediaMarqueeBase;
    bool mediaSelected(core::Id id) const { return std::find(_mediaSelection.begin(), _mediaSelection.end(), id) != _mediaSelection.end(); }

    // Folders of the media panel: the selected one (imports go there), the
    // one renamed in place.
    std::string _selectedFolder;
    std::string _renamingFolder;
    std::string _folderName;
    bool _focusFolderName = false;

    TimelineView _timeline;
    core::Time _playhead = 0;
    bool _playing = false;

    // The audio mix is rebuilt when the project or the media change.
    bool _mixDirty = true;
    uint64_t _seenMediaRevision = 0;

    std::string _windowTitle;

    std::mutex _errorMutex;
    bool _hasErrorMessage = false;
    std::string _errorMessage;

    // Results of the file dialogs. SDL may call the dialog callbacks from
    // another thread: they are processed on the main thread, between two
    // frames (see eventBeforeFrame()).
    std::mutex _dialogMutex;
    std::filesystem::path _dialogOpenPath;
    std::filesystem::path _dialogSavePath;
    std::vector<std::filesystem::path> _dialogImportPaths;
    std::filesystem::path _dialogExportPath;

    // Action waiting for the answer to the unsaved changes popup, then for
    // the project to be saved. See confirmThen().
    PendingAction _pendingAction = PendingAction::None;
    PendingAction _requestedAction = PendingAction::None;       // From the menus
    std::filesystem::path _pendingPath;
    bool _actionAfterSave = false;
    bool _requestUnsavedChangesPopup = false;

    // Ensures the default docking layout is only built on first launch.
    bool _dockLayoutInitialized = false;

    static constexpr SDL_DialogFileFilter projectFilters[] = {
        { "QuickEdit project", "qep" }
    };

    static constexpr SDL_DialogFileFilter subtitleFilters[] = {
        { "SubRip subtitles", "srt" },
    };

    static constexpr SDL_DialogFileFilter exportFilters[] = {
        { "MP4 video", "mp4" },
        { "QuickTime video", "mov" },
        { "Matroska video", "mkv" }
    };

    static constexpr SDL_DialogFileFilter mediaFilters[] = {
        { "Media", "mp4;mov;mkv;webm;avi;mxf;m4v;wav;mp3;flac;ogg;opus;aac;m4a;aiff;aif;png;jpg;jpeg;bmp;tga;tif;tiff;exr;webp" },
        { "Video", "mp4;mov;mkv;webm;avi;mxf;m4v" },
        { "Audio", "wav;mp3;flac;ogg;opus;aac;m4a;aiff;aif" },
        { "Image", "png;jpg;jpeg;bmp;tga;tif;tiff;exr;webp" },
        { "All files", "*" }
    };
};

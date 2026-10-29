#pragma once

#include <SDL3/SDL.h>

#include <memory>
#include <mutex>
#include <string>
#include <vector>


namespace audio
{
    // A VST3 effect installed on the system.
    struct PluginFile
    {
        std::string path;       // .vst3 bundle or file, UTF-8
        std::string name;       // File name without extension
    };

    // The .vst3 files of the standard folders, without loading them.
    std::vector<PluginFile> findVst3Plugins();


    // An instance of a VST3 audio effect, processing stereo.
    //
    // load(), state, and the editor are for the main thread; process() for
    // the audio thread (or the export thread), once prepare()d. Parameter
    // changes made in the editor reach the processing at its next block.
    class Vst3Plugin
    {
    public:
        // The audio effect class classId (hexadecimal) of the file, or its
        // first audio effect when classId is empty. nullptr and error on
        // failure.
        static std::unique_ptr<Vst3Plugin> load(const std::string& path, const std::string& classId, std::string& error);

        ~Vst3Plugin();

        Vst3Plugin(const Vst3Plugin&) = delete;
        Vst3Plugin& operator=(const Vst3Plugin&) = delete;

        const std::string& name() const { return _name; }
        const std::string& classId() const { return _classId; }

        // Activates the processing. maxFrames: largest block given to
        // process().
        bool prepare(double sampleRate, int maxFrames);

        // Processes interleaved stereo samples in place. Real-time safe.
        void process(float* samples, int frames, int64_t position);

        // State of the plugin (its parameters), to save in the project.
        std::vector<uint8_t> saveState();
        bool loadState(const std::vector<uint8_t>& state);

        // Editor of the plugin, in its own window.
        bool hasEditor() const;
        bool openEditor();
        void closeEditor();
        bool editorOpen() const { return _window != nullptr; }

        // Events of the editor window (close, resize). Returns true when it
        // was for this plugin.
        bool handleEvent(const SDL_Event& event);

    private:
        Vst3Plugin() = default;

        struct Impl;
        std::unique_ptr<Impl> _impl;

        std::string _name;
        std::string _classId;
        SDL_Window* _window = nullptr;
    };
}

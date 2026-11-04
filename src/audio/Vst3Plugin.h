#pragma once

#include <SDL3/SDL.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
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


    // An audio effect of a plugin file. A file may hold several (a shell,
    // such as the WaveShell of Waves, holds all the plugins of the brand).
    struct PluginClass
    {
        std::string path;           // Of the file, UTF-8
        std::string classId;        // Hexadecimal
        std::string name;
        std::string vendor;
        std::string subCategories;  // "Fx|Delay"...
    };

    // The audio effects of a plugin file, by loading its module. Empty with
    // error set when it cannot be loaded.
    std::vector<PluginClass> listPluginClasses(const std::string& path, std::string& error);


    // The audio effects of all the installed plugins, found on a thread of
    // its own. The files seen before (same date and size) come from a cache
    // in the settings folder, so that only new or updated ones are loaded.
    class PluginScanner
    {
    public:
        PluginScanner() = default;
        ~PluginScanner();

        PluginScanner(const PluginScanner&) = delete;
        PluginScanner& operator=(const PluginScanner&) = delete;

        // Looks for the plugins, unless already doing so. rescan: loads
        // every file again.
        void start(bool rescan = false);

        bool started() const { return _thread.joinable(); }
        bool scanning() const { return _scanning.load(); }

        // The effects found by the last scan, sorted by name.
        std::vector<PluginClass> classes() const;

        // The file being loaded, while scanning.
        std::string current() const;

    private:
        void run(bool rescan);

        std::thread _thread;
        std::atomic<bool> _scanning = false;
        std::atomic<bool> _stop = false;

        mutable std::mutex _mutex;
        std::vector<PluginClass> _classes;
        std::string _current;
    };


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

        // The plugin crashed in its own code (access violation, exception):
        // it was reported and is no longer called, the sound goes through
        // it unchanged. Windows only for now: elsewhere a crash in a plugin
        // still ends QuickEdit.
        bool crashed() const { return _crashed; }

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

        // The calls into the plugin, guarded by the public functions.
        static std::unique_ptr<Vst3Plugin> loadUnguarded(const std::string& path, const std::string& classId, std::string& error);
        bool prepareUnguarded(double sampleRate, int maxFrames);
        void processUnguarded(float* samples, int frames, int64_t position);
        std::vector<uint8_t> saveStateUnguarded();
        bool loadStateUnguarded(const std::vector<uint8_t>& state);
        bool openEditorUnguarded();
        void closeEditorUnguarded();

        // Reports a crash during an action, once, and turns the plugin off.
        void crash(const char* during);

        std::atomic<bool> _crashed { false };

        struct Impl;
        std::unique_ptr<Impl> _impl;

        std::string _name;
        std::string _classId;
        SDL_Window* _window = nullptr;
    };
}

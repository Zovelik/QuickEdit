#pragma once

#include <audio/Mixer.h>

#include <core/Project.h>

#include <SDL3/SDL.h>

#include <map>
#include <memory>
#include <string>
#include <vector>


namespace audio { class Vst3Plugin; }


// The live instances of the effects (VST3 plugins) of the audio tracks and
// of their clips.
//
// sync() follows the effect slots of the project: it loads the plugins of
// new slots with their saved state, and drops the instances of removed ones.
// The instances are shared with the audio thread through the mix graph; a
// dropped one is released a few seconds later, never by the audio thread.
// Main thread only.
class EffectRack
{
public:
    // Largest block processed at once (see AudioEngine and Exporter).
    static constexpr int MaxBlockFrames = 1024;

    ~EffectRack();

    // Returns true when the instances changed (the mix graph must be
    // rebuilt).
    bool sync(const core::Project& project);

    // Effects of a track or a clip for the mix graph: the instances of its
    // slots not bypassed.
    std::vector<std::shared_ptr<audio::AudioEffect>> effectsOf(const std::vector<core::EffectSlot>& slots) const;

    audio::Vst3Plugin* plugin(core::Id slotId) const;
    std::string error(core::Id slotId) const;

    // Copies the states of the instances (parameters changed in their
    // editors) into the slots of the project. Not an edit for the history.
    void saveStates(core::Project& project) const;

    // Fills the class id and the name of the slots added without them, from
    // their loaded plugins. Returns true when a slot changed.
    bool completeSlots(core::Project& project) const;

    // Events of the editor windows.
    bool handleEvent(const SDL_Event& event);

    // New instances of the effects of a project, with their states, for an
    // export: per owner (track or clip) id.
    static std::map<core::Id, std::vector<std::shared_ptr<audio::AudioEffect>>> createForExport(const core::Project& project);

private:
    struct Instance
    {
        // Plugin of the instance: another one in the slot reloads it. The
        // class id is the one loaded when the slot gave none.
        std::string path;
        std::string classId;
        std::shared_ptr<audio::AudioEffect> effect;
        audio::Vst3Plugin* plugin = nullptr;
        std::string error;
    };

    std::map<core::Id, Instance> _instances;
    uint32_t _sampleRate = 0;

    // Dropped instances, with the time they were dropped.
    std::vector<std::pair<std::shared_ptr<audio::AudioEffect>, Uint64>> _graveyard;
};

#include <app/EffectRack.h>

#include <audio/Vst3Plugin.h>

#include <core/Base64.h>

#include <algorithm>
#include <functional>


using namespace core;


namespace {

// A plugin as an effect of the mix graph.
class PluginEffect : public audio::AudioEffect
{
public:
    explicit PluginEffect(std::unique_ptr<audio::Vst3Plugin> plugin) : _plugin(std::move(plugin)) {}

    void process(float* samples, int frames, int64_t position) override
    {
        _plugin->process(samples, frames, position);
    }

    audio::Vst3Plugin* plugin() const { return _plugin.get(); }

private:
    std::unique_ptr<audio::Vst3Plugin> _plugin;
};


// Loads the plugin of a slot, with its state, ready to process.
std::shared_ptr<PluginEffect> loadSlot(const EffectSlot& slot, uint32_t sampleRate, std::string& error)
{
    std::unique_ptr<audio::Vst3Plugin> plugin = audio::Vst3Plugin::load(slot.path, slot.classId, error);

    if (!plugin) {
        return nullptr;
    }

    if (!slot.state.empty()) {
        plugin->loadState(base64Decode(slot.state));
    }

    if (!plugin->prepare((double)sampleRate, EffectRack::MaxBlockFrames)) {
        error = "The plugin refused the audio format";
        return nullptr;
    }

    return std::make_shared<PluginEffect>(std::move(plugin));
}


// Released this long after being dropped: the audio thread is done with
// the graphs that held it.
constexpr Uint64 GraveyardDelayNS = 3 * SDL_NS_PER_SECOND;

// The tracks that can have effects: sound and audio tracks.
std::vector<std::reference_wrapper<Track>> allSoundTracks(Project& project)
{
    std::vector<std::reference_wrapper<Track>> tracks;
    for (Track& track : project.sequence().soundTracks) tracks.push_back(track);
    for (Track& track : project.sequence().audioTracks) tracks.push_back(track);
    return tracks;
}


std::vector<std::reference_wrapper<const Track>> allSoundTracks(const Project& project)
{
    std::vector<std::reference_wrapper<const Track>> tracks;
    for (const Track& track : project.sequence().soundTracks) tracks.push_back(track);
    for (const Track& track : project.sequence().audioTracks) tracks.push_back(track);
    return tracks;
}

} // namespace


EffectRack::~EffectRack()
{
    // Editors first, then the plugins.
    for (auto& [id, instance] : _instances) {
        if (instance.plugin) {
            instance.plugin->closeEditor();
        }
    }
}


bool EffectRack::sync(const Project& project)
{
    const Uint64 now = SDL_GetTicksNS();
    bool changed = false;

    std::erase_if(_graveyard, [&](const auto& entry) { return now - entry.second > GraveyardDelayNS; });

    // A new sample rate: every plugin is prepared again, by reloading it.
    const uint32_t sampleRate = project.sequence().sampleRate;
    const bool newRate = sampleRate != _sampleRate;
    _sampleRate = sampleRate;

    std::map<Id, const EffectSlot*> slots;

    for (const Track& track : allSoundTracks(project)) {
        for (const EffectSlot& slot : track.effects) {
            slots[slot.id] = &slot;
        }
    }

    // Removed slots, or other plugins in them.
    for (auto it = _instances.begin(); it != _instances.end();) {
        const auto slot = slots.find(it->first);

        const bool samePlugin = slot != slots.end() && slot->second->path == it->second.path &&
            (slot->second->classId.empty() || slot->second->classId == it->second.classId);

        if (!samePlugin || newRate) {
            if (it->second.plugin) {
                it->second.plugin->closeEditor();
            }

            if (it->second.effect) {
                _graveyard.emplace_back(it->second.effect, now);
            }

            it = _instances.erase(it);
            changed = true;
        }
        else {
            ++it;
        }
    }

    // New slots.
    for (const auto& [id, slot] : slots) {
        if (_instances.count(id)) {
            continue;
        }

        Instance instance;
        instance.path = slot->path;
        instance.classId = slot->classId;

        std::string error;
        std::shared_ptr<PluginEffect> effect = loadSlot(*slot, sampleRate, error);

        if (effect) {
            instance.plugin = effect->plugin();
            instance.classId = instance.plugin->classId();
            instance.effect = std::move(effect);
        }
        else {
            instance.error = error.empty() ? "Could not load the plugin" : error;
            SDL_Log("Effect %s: %s", slot->name.c_str(), instance.error.c_str());
        }

        _instances[id] = std::move(instance);
        changed = true;
    }

    return changed;
}


std::vector<std::shared_ptr<audio::AudioEffect>> EffectRack::effectsOf(const Track& track) const
{
    std::vector<std::shared_ptr<audio::AudioEffect>> effects;

    for (const EffectSlot& slot : track.effects) {
        const auto it = _instances.find(slot.id);

        if (!slot.bypass && it != _instances.end() && it->second.effect) {
            effects.push_back(it->second.effect);
        }
    }

    return effects;
}


audio::Vst3Plugin* EffectRack::plugin(Id slotId) const
{
    const auto it = _instances.find(slotId);
    return (it != _instances.end()) ? it->second.plugin : nullptr;
}


std::string EffectRack::error(Id slotId) const
{
    const auto it = _instances.find(slotId);
    return (it != _instances.end()) ? it->second.error : std::string();
}


void EffectRack::saveStates(Project& project) const
{
    for (Track& track : allSoundTracks(project)) {
        for (EffectSlot& slot : track.effects) {
            if (audio::Vst3Plugin* instance = plugin(slot.id)) {
                slot.state = base64Encode(instance->saveState());
            }
        }
    }
}


bool EffectRack::completeSlots(Project& project) const
{
    bool changed = false;

    for (Track& track : allSoundTracks(project)) {
        for (EffectSlot& slot : track.effects) {
            audio::Vst3Plugin* instance = plugin(slot.id);

            if (instance && slot.classId.empty()) {
                slot.classId = instance->classId();
                slot.name = instance->name();
                changed = true;
            }
        }
    }

    return changed;
}


bool EffectRack::handleEvent(const SDL_Event& event)
{
    for (auto& [id, instance] : _instances) {
        if (instance.plugin && instance.plugin->handleEvent(event)) {
            return true;
        }
    }

    return false;
}


std::map<Id, std::vector<std::shared_ptr<audio::AudioEffect>>> EffectRack::createForExport(const Project& project)
{
    std::map<Id, std::vector<std::shared_ptr<audio::AudioEffect>>> effects;

    for (const Track& track : allSoundTracks(project)) {
        for (const EffectSlot& slot : track.effects) {
            if (slot.bypass) {
                continue;
            }

            std::string error;

            if (std::shared_ptr<PluginEffect> effect = loadSlot(slot, project.sequence().sampleRate, error)) {
                effects[track.id].push_back(std::move(effect));
            }
        }
    }

    return effects;
}

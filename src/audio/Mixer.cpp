#include <audio/Mixer.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <set>


using namespace core;


namespace audio
{

namespace {

// Length of the fades at the edges of the clips.
constexpr double EdgeFadeSeconds = 0.003;


int64_t timeToFrame(Time t, uint32_t sampleRate)
{
    return mulDivFloor(t, sampleRate, TicksPerSecond);
}

} // namespace


// Mixes clips into output (added to it).
void mixClips(const MixGraph& graph, const std::vector<MixClip>& clips, int64_t first, int64_t count, float* output);
void mixClip(const MixGraph& graph, const MixClip& clip, int64_t first, int64_t count, float* output);
void mixTrack(const MixGraph& graph, const MixTrack& track, int64_t first, int64_t count, float* output, size_t depth);


namespace {

using AudioOf = std::function<std::shared_ptr<const media::AudioBuffer>(Id mediaId, int stream)>;
using EffectsOf = std::function<std::vector<std::shared_ptr<AudioEffect>>(Id owner, const std::vector<EffectSlot>& slots)>;


// The effects of an owner (track or clip), once per graph: a plugin
// processes a single stream.
std::vector<std::shared_ptr<AudioEffect>> effectsOnce(const EffectsOf& effectsOf, Id owner,
    const std::vector<EffectSlot>& slots, std::set<Id>& usedOwners)
{
    if (!effectsOf || slots.empty() || !usedOwners.insert(owner).second) {
        return {};
    }

    return effectsOf(owner, slots);
}


// The sound of a nested sequence, shown in the parent from time from to to,
// a time t of it being t + offset in the parent: its sound and audio
// tracks (and those of the sequences nested in it), each with its effects,
// as sub-tracks of a track of the mix.
void addNestedSound(const Project& project, const Sequence& nested, Time offset, Time from, Time to,
    uint32_t sampleRate, MixTrack& mixTrack, const AudioOf& audioOf, const EffectsOf& effectsOf,
    std::set<Id>& usedOwners, int depth)
{
    if (depth > 8 || to <= from) {
        return;
    }

    std::vector<const Track*> tracks;
    for (const Track& track : nested.soundTracks) tracks.push_back(&track);
    for (const Track& track : nested.audioTracks) tracks.push_back(&track);

    const bool anySolo = std::any_of(tracks.begin(), tracks.end(), [](const Track* track) { return track->solo && !track->muted; });

    for (const Track* track : tracks) {
        if (track->muted || (anySolo && !track->solo)) {
            continue;
        }

        MixTrack subTrack;

        for (const Clip& clip : track->clips) {
            if (clip.muted) {
                continue;
            }

            const Time start = std::max(clip.start + offset, from);
            const Time end = std::min(clip.end() + offset, to);
            std::shared_ptr<const media::AudioBuffer> buffer = (end > start) ? audioOf(clip.mediaId, clip.audioSource()) : nullptr;

            if (!buffer || buffer->sampleRate != sampleRate) {
                continue;
            }

            MixClip mixClip;
            mixClip.start = timeToFrame(start, sampleRate);
            mixClip.length = timeToFrame(end, sampleRate) - mixClip.start;
            mixClip.sourceStart = timeToFrame(clip.sourceAt(start - (clip.start + offset)), sampleRate);
            mixClip.speed = clip.speed;
            mixClip.gain = std::pow(10.f, clip.gainDb / 20.f);

            // The fades where the clip is not cut by the parent.
            mixClip.fadeIn = (start == clip.start + offset) ? timeToFrame(clip.fadeIn, sampleRate) : 0;
            mixClip.fadeOut = (end == clip.end() + offset) ? timeToFrame(clip.fadeOut, sampleRate) : 0;
            mixClip.buffer = std::move(buffer);

            if (mixClip.length > 0) {
                mixClip.effects = effectsOnce(effectsOf, clip.id, clip.effects, usedOwners);
                subTrack.clips.push_back(std::move(mixClip));
            }
        }

        // Deeper: the sequences on the video track of a sound track.
        if (track->type == TrackType::Sound) {
            const size_t index = (size_t)(track - nested.soundTracks.data());

            if (index < nested.videoTracks.size()) {
                for (const Clip& clip : nested.videoTracks[index].clips) {
                    const Sequence* inner = (clip.kind == ClipKind::Sequence) ? project.findSequence(clip.sequence) : nullptr;

                    if (inner) {
                        addNestedSound(project, *inner, offset + clip.start - clip.sourceIn,
                            std::max(clip.start + offset, from), std::min(clip.end() + offset, to),
                            sampleRate, subTrack, audioOf, effectsOf, usedOwners, depth + 1);
                    }
                }
            }
        }

        if (!subTrack.clips.empty() || !subTrack.subTracks.empty()) {
            subTrack.effects = effectsOnce(effectsOf, track->id, track->effects, usedOwners);
            mixTrack.subTracks.push_back(std::move(subTrack));
        }
    }
}

} // namespace


std::shared_ptr<const MixGraph> buildMixGraph(
    const Project& project,
    const std::function<std::shared_ptr<const media::AudioBuffer>(Id mediaId, int stream)>& audioOf,
    const std::function<std::vector<std::shared_ptr<AudioEffect>>(Id owner, const std::vector<EffectSlot>& slots)>& effectsOf)
{
    auto graph = std::make_shared<MixGraph>();
    const Sequence& sequence = project.sequence();
    graph->sampleRate = sequence.sampleRate;

    std::set<Id> usedOwners;

    std::vector<const Track*> tracks;

    for (const Track& track : sequence.soundTracks) tracks.push_back(&track);
    for (const Track& track : sequence.audioTracks) tracks.push_back(&track);

    const bool anySolo = std::any_of(tracks.begin(), tracks.end(),
        [](const Track* track) { return track->solo && !track->muted; });

    for (const Track* trackPointer : tracks) {
        const Track& track = *trackPointer;

        if (track.muted || (anySolo && !track.solo)) {
            continue;
        }

        MixTrack& mixTrack = graph->tracks.emplace_back();

        mixTrack.effects = effectsOnce(effectsOf, track.id, track.effects, usedOwners);

        for (const Clip& clip : track.clips) {
            if (clip.muted) {
                continue;
            }

            std::shared_ptr<const media::AudioBuffer> buffer = audioOf(clip.mediaId, clip.audioSource());

            if (!buffer || buffer->sampleRate != graph->sampleRate) {
                continue;
            }

            MixClip mixClip;
            mixClip.start = timeToFrame(clip.start, graph->sampleRate);
            mixClip.length = timeToFrame(clip.end(), graph->sampleRate) - mixClip.start;
            mixClip.sourceStart = timeToFrame(clip.sourceIn, graph->sampleRate);
            mixClip.speed = clip.speed;
            mixClip.gain = std::pow(10.f, clip.gainDb / 20.f);
            mixClip.fadeIn = timeToFrame(clip.fadeIn, graph->sampleRate);
            mixClip.fadeOut = timeToFrame(clip.fadeOut, graph->sampleRate);
            mixClip.buffer = std::move(buffer);

            mixClip.effects = effectsOnce(effectsOf, clip.id, clip.effects, usedOwners);

            if (mixClip.length > 0) {
                mixTrack.clips.push_back(std::move(mixClip));
            }
        }

        // The sound of the nested sequences of the video track of a sound
        // track, with it.
        if (track.type == TrackType::Sound) {
            for (size_t i = 0; i < sequence.soundTracks.size() && i < sequence.videoTracks.size(); i++) {
                if (&sequence.soundTracks[i] != &track) {
                    continue;
                }

                for (const Clip& clip : sequence.videoTracks[i].clips) {
                    const Sequence* nested = (clip.kind == ClipKind::Sequence) ? project.findSequence(clip.sequence) : nullptr;

                    if (nested) {
                        addNestedSound(project, *nested, clip.start - clip.sourceIn, clip.start, clip.end(),
                            graph->sampleRate, mixTrack, audioOf, effectsOf, usedOwners, 1);
                    }
                }
            }
        }
    }

    return graph;
}


// Mixes a track into output (added to it): its clips and sub-tracks, then
// its effects. One scratch buffer per depth.
void mixTrack(const MixGraph& graph, const MixTrack& track, int64_t first, int64_t count, float* output, size_t depth)
{
    constexpr int64_t channels = media::AudioBuffer::Channels;
    constexpr size_t MaxDepth = 12;

    thread_local std::vector<std::vector<float>> scratches(MaxDepth);

    if (depth >= MaxDepth) {
        return;
    }

    std::vector<float>& scratch = scratches[depth];

    if (scratch.size() < (size_t)(count * channels)) {
        scratch.resize((size_t)(count * channels));
    }

    float* trackOutput = scratch.data();
    std::memset(trackOutput, 0, (size_t)(count * channels) * sizeof(float));

    mixClips(graph, track.clips, first, count, trackOutput);

    for (const MixTrack& subTrack : track.subTracks) {
        mixTrack(graph, subTrack, first, count, trackOutput, depth + 1);
    }

    for (const std::shared_ptr<AudioEffect>& effect : track.effects) {
        effect->process(trackOutput, (int)count, first);
    }

    for (int64_t i = 0; i < count * channels; i++) {
        output[i] += trackOutput[i];
    }
}


void mix(const MixGraph& graph, int64_t first, int64_t count, float* output)
{
    constexpr int64_t channels = media::AudioBuffer::Channels;

    std::memset(output, 0, (size_t)(count * channels) * sizeof(float));

    // Each track in a scratch buffer, through its effects, added to the
    // output.
    for (const MixTrack& track : graph.tracks) {
        mixTrack(graph, track, first, count, output, 0);
    }

    // Hard limit: the output must never wrap around.
    for (int64_t i = 0; i < count * channels; i++) {
        output[i] = std::clamp(output[i], -1.f, 1.f);
    }
}


void mixClips(const MixGraph& graph, const std::vector<MixClip>& clips, int64_t first, int64_t count, float* output)
{
    constexpr int64_t channels = media::AudioBuffer::Channels;

    // A clip with effects: alone in a buffer of its own, through them, then
    // added.
    thread_local std::vector<float> clipScratch;

    for (const MixClip& clip : clips) {
        if (clip.start + clip.length <= first || clip.start >= first + count) {
            continue;
        }

        if (clip.effects.empty()) {
            mixClip(graph, clip, first, count, output);
            continue;
        }

        if (clipScratch.size() < (size_t)(count * channels)) {
            clipScratch.resize((size_t)(count * channels));
        }

        std::memset(clipScratch.data(), 0, (size_t)(count * channels) * sizeof(float));
        mixClip(graph, clip, first, count, clipScratch.data());

        for (const std::shared_ptr<AudioEffect>& effect : clip.effects) {
            effect->process(clipScratch.data(), (int)count, first);
        }

        for (int64_t i = 0; i < count * channels; i++) {
            output[i] += clipScratch[i];
        }
    }
}


void mixClip(const MixGraph& graph, const MixClip& clip, int64_t first, int64_t count, float* output)
{
    constexpr int64_t channels = media::AudioBuffer::Channels;

    const int64_t fadeLength = std::max<int64_t>(1, (int64_t)(EdgeFadeSeconds * graph.sampleRate));
    const int64_t last = first + count;

    {
        const int64_t clipEnd = clip.start + clip.length;

        if (clipEnd <= first || clip.start >= last) {
            return;
        }

        const int64_t from = std::max(first, clip.start);
        const int64_t to = std::min(last, clipEnd);
        const float* source = clip.buffer->samples.data();
        const int64_t sourceFrames = clip.buffer->frameCount();

        const bool resampled = clip.speed != 1.0;

        for (int64_t frame = from; frame < to; frame++) {
            // Another speed: between two frames of the source, linearly.
            const double sourcePosition = (double)clip.sourceStart + (double)(frame - clip.start) * clip.speed;
            const int64_t sourceFrame = resampled ? (int64_t)std::floor(sourcePosition) : clip.sourceStart + (frame - clip.start);

            if (sourceFrame < 0 || sourceFrame >= sourceFrames) {
                continue;
            }

            // Fade in and out over the first and last frames of the clip,
            // against clicks.
            const int64_t fromStart = frame - clip.start;
            const int64_t fromEnd = clipEnd - 1 - frame;
            const int64_t fromEdge = std::min(fromStart, fromEnd);
            float fade = (fromEdge < fadeLength) ? (float)(fromEdge + 1) / (float)(fadeLength + 1) : 1.f;

            // Fades of the clip, at constant power: a fade out against a
            // fade in sounds even.
            if (fromStart < clip.fadeIn) {
                fade *= std::sin(0.5f * 3.14159265f * (float)fromStart / (float)clip.fadeIn);
            }

            if (fromEnd < clip.fadeOut) {
                fade *= std::sin(0.5f * 3.14159265f * (float)fromEnd / (float)clip.fadeOut);
            }

            const float gain = clip.gain * fade;

            float* out = output + (frame - first) * channels;
            const float* in = source + sourceFrame * channels;

            if (resampled && sourceFrame + 1 < sourceFrames) {
                const float t = (float)(sourcePosition - (double)sourceFrame);
                out[0] += (in[0] + (in[channels] - in[0]) * t) * gain;
                out[1] += (in[1] + (in[channels + 1] - in[1]) * t) * gain;
            }
            else {
                out[0] += in[0] * gain;
                out[1] += in[1] * gain;
            }
        }
    }
}

}

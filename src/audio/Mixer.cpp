#include <audio/Mixer.h>

#include <algorithm>
#include <cmath>
#include <cstring>


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


namespace {

using AudioOf = std::function<std::shared_ptr<const media::AudioBuffer>(Id mediaId)>;


// The sound of a nested sequence, shown in the parent from time from to to,
// a time t of it being t + offset in the parent: its sound and audio
// clips (and those of the sequences nested in it) into a track of the mix.
void addNestedSound(const Project& project, const Sequence& nested, Time offset, Time from, Time to,
    uint32_t sampleRate, MixTrack& mixTrack, const AudioOf& audioOf, int depth)
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

        for (const Clip& clip : track->clips) {
            const Time start = std::max(clip.start + offset, from);
            const Time end = std::min(clip.end() + offset, to);
            std::shared_ptr<const media::AudioBuffer> buffer = (end > start) ? audioOf(clip.mediaId) : nullptr;

            if (!buffer || buffer->sampleRate != sampleRate) {
                continue;
            }

            MixClip mixClip;
            mixClip.start = timeToFrame(start, sampleRate);
            mixClip.length = timeToFrame(end, sampleRate) - mixClip.start;
            mixClip.sourceStart = timeToFrame(clip.sourceIn + (start - (clip.start + offset)), sampleRate);
            mixClip.gain = std::pow(10.f, clip.gainDb / 20.f);

            // The fades where the clip is not cut by the parent.
            mixClip.fadeIn = (start == clip.start + offset) ? timeToFrame(clip.fadeIn, sampleRate) : 0;
            mixClip.fadeOut = (end == clip.end() + offset) ? timeToFrame(clip.fadeOut, sampleRate) : 0;
            mixClip.buffer = std::move(buffer);

            if (mixClip.length > 0) {
                mixTrack.clips.push_back(std::move(mixClip));
            }
        }
    }

    // Deeper
    for (const Track& track : nested.videoTracks) {
        for (const Clip& clip : track.clips) {
            const Sequence* inner = (clip.kind == ClipKind::Sequence) ? project.findSequence(clip.sequence) : nullptr;

            if (inner) {
                addNestedSound(project, *inner, offset + clip.start - clip.sourceIn,
                    std::max(clip.start + offset, from), std::min(clip.end() + offset, to), sampleRate, mixTrack, audioOf, depth + 1);
            }
        }
    }
}

} // namespace


std::shared_ptr<const MixGraph> buildMixGraph(
    const Project& project,
    const std::function<std::shared_ptr<const media::AudioBuffer>(Id mediaId)>& audioOf,
    const std::function<std::vector<std::shared_ptr<AudioEffect>>(Id owner, const std::vector<EffectSlot>& slots)>& effectsOf)
{
    auto graph = std::make_shared<MixGraph>();
    const Sequence& sequence = project.sequence();
    graph->sampleRate = sequence.sampleRate;

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

        if (effectsOf) {
            mixTrack.effects = effectsOf(track.id, track.effects);
        }

        for (const Clip& clip : track.clips) {
            std::shared_ptr<const media::AudioBuffer> buffer = audioOf(clip.mediaId);

            if (!buffer || buffer->sampleRate != graph->sampleRate) {
                continue;
            }

            MixClip mixClip;
            mixClip.start = timeToFrame(clip.start, graph->sampleRate);
            mixClip.length = timeToFrame(clip.end(), graph->sampleRate) - mixClip.start;
            mixClip.sourceStart = timeToFrame(clip.sourceIn, graph->sampleRate);
            mixClip.gain = std::pow(10.f, clip.gainDb / 20.f);
            mixClip.fadeIn = timeToFrame(clip.fadeIn, graph->sampleRate);
            mixClip.fadeOut = timeToFrame(clip.fadeOut, graph->sampleRate);
            mixClip.buffer = std::move(buffer);

            if (effectsOf && !clip.effects.empty()) {
                mixClip.effects = effectsOf(clip.id, clip.effects);
            }

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
                            graph->sampleRate, mixTrack, audioOf, 1);
                    }
                }
            }
        }
    }

    return graph;
}


void mix(const MixGraph& graph, int64_t first, int64_t count, float* output)
{
    constexpr int64_t channels = media::AudioBuffer::Channels;

    std::memset(output, 0, (size_t)(count * channels) * sizeof(float));

    // Each track in a scratch buffer, through its effects, added to the
    // output.
    thread_local std::vector<float> scratch;

    if (scratch.size() < (size_t)(count * channels)) {
        scratch.resize((size_t)(count * channels));
    }

    for (const MixTrack& track : graph.tracks) {
        float* trackOutput = scratch.data();
        std::memset(trackOutput, 0, (size_t)(count * channels) * sizeof(float));

        mixClips(graph, track.clips, first, count, trackOutput);

        for (const std::shared_ptr<AudioEffect>& effect : track.effects) {
            effect->process(trackOutput, (int)count, first);
        }

        for (int64_t i = 0; i < count * channels; i++) {
            output[i] += trackOutput[i];
        }
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

        for (int64_t frame = from; frame < to; frame++) {
            const int64_t sourceFrame = clip.sourceStart + (frame - clip.start);

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

            out[0] += in[0] * gain;
            out[1] += in[1] * gain;
        }
    }
}

}

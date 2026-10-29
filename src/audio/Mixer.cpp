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


std::shared_ptr<const MixGraph> buildMixGraph(
    const Project& project,
    const std::function<std::shared_ptr<const media::AudioBuffer>(Id mediaId)>& audioOf,
    const std::function<std::vector<std::shared_ptr<AudioEffect>>(const Track& track)>& effectsOf)
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
            mixTrack.effects = effectsOf(track);
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

            if (mixClip.length > 0) {
                mixTrack.clips.push_back(std::move(mixClip));
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

    const int64_t fadeLength = std::max<int64_t>(1, (int64_t)(EdgeFadeSeconds * graph.sampleRate));
    const int64_t last = first + count;

    for (const MixClip& clip : clips) {
        const int64_t clipEnd = clip.start + clip.length;

        if (clipEnd <= first || clip.start >= last) {
            continue;
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

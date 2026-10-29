#include <core/Layers.h>

#include <algorithm>


namespace core
{

namespace {

LayerRequest makeLayer(const Clip& clip, size_t track, int slot, Time localTime, bool still)
{
    LayerRequest layer;
    layer.clip = &clip;
    layer.track = track;
    layer.slot = slot;
    layer.localTime = std::clamp<Time>(localTime, 0, std::max<Time>(0, clip.duration - 1));
    layer.sourceTime = clip.frozen ? clip.sourceIn : clip.sourceIn + layer.localTime;
    layer.still = still || clip.frozen;
    return layer;
}


// Look of the two pictures of a transition at progress p in [0, 1].
void applyTransition(TransitionType type, float p, float width, LayerRequest* outgoing, LayerRequest& incoming)
{
    const float eased = applyEasing(Easing::EaseInOut, p);

    switch (type) {
        case TransitionType::Dissolve:
            incoming.opacity = eased;
            break;

        case TransitionType::DipToBlack:
            // Out over the first half, in over the second.
            if (outgoing) {
                outgoing->opacity = std::clamp(1.f - 2.f * p, 0.f, 1.f);
            }

            incoming.opacity = std::clamp(2.f * p - 1.f, 0.f, 1.f);
            break;

        case TransitionType::Push:
            if (outgoing) {
                outgoing->offsetX = -eased * width;
            }

            incoming.offsetX = (1.f - eased) * width;
            break;

        case TransitionType::Wipe:
            incoming.wipe = eased;
            break;

        case TransitionType::Zoom:
            incoming.scale = 0.6f + 0.4f * applyEasing(Easing::EaseOut, p);
            incoming.opacity = applyEasing(Easing::EaseOut, p);
            break;

        default:
            break;
    }
}

} // namespace


const char* transitionName(TransitionType type)
{
    switch (type) {
        case TransitionType::Dissolve:   return "Dissolve";
        case TransitionType::DipToBlack: return "Dip to Black";
        case TransitionType::Push:       return "Push";
        case TransitionType::Wipe:       return "Wipe";
        case TransitionType::Zoom:       return "Zoom";
        default:                         return "None";
    }
}


std::vector<LayerRequest> layersAt(const Sequence& sequence, Time t)
{
    std::vector<LayerRequest> layers;

    for (size_t i = 0; i < sequence.videoTracks.size(); i++) {
        const Track& track = sequence.videoTracks[i];

        if (track.muted) {
            continue;
        }

        const int index = track.clipAt(t);
        const Clip* current = (index >= 0) ? &track.clips[index] : nullptr;

        // The clip ending exactly where another one starts.
        auto clipEndingAt = [&](Time time) -> const Clip* {
            for (const Clip& clip : track.clips) {
                if (clip.end() == time) return &clip;
            }
            return nullptr;
        };

        // Duration of the transition into a clip, within both clips.
        auto transitionLength = [&](const Clip& incoming, const Clip* outgoing) {
            Time duration = std::min(incoming.transitionIn.duration, incoming.duration);

            if (outgoing) {
                duration = std::min(duration, outgoing->duration);
            }

            return std::max<Time>(1, duration);
        };

        const Clip* incoming = nullptr;
        const Clip* outgoing = nullptr;
        Time length = 0;

        // After the cut: the current clip is coming in.
        if (current && current->transitionIn.type != TransitionType::None) {
            const Clip* before = clipEndingAt(current->start);
            const Time duration = transitionLength(*current, before);

            if (t < current->start + duration / 2) {
                incoming = current;
                outgoing = before;
                length = duration;
            }
        }

        // Before the cut: the next clip is already coming in.
        if (!incoming) {
            const Clip* next = nullptr;

            for (const Clip& clip : track.clips) {
                if (clip.start > t && (!next || clip.start < next->start)) {
                    next = &clip;
                }
            }

            if (next && next->transitionIn.type != TransitionType::None) {
                const Clip* before = (current && current->end() == next->start) ? current : nullptr;
                const Time duration = transitionLength(*next, before);

                // A gap before the next clip: it only starts to appear when
                // nothing is shown on the track.
                if (t >= next->start - duration / 2 && (before || !current)) {
                    incoming = next;
                    outgoing = before;
                    length = duration;
                }
            }
        }

        if (!incoming) {
            if (current) {
                layers.push_back(makeLayer(*current, i, 0, t - current->start, false));
            }

            continue;
        }

        const Time cut = incoming->start;
        const float p = std::clamp((float)((double)(t - (cut - length / 2)) / (double)length), 0.f, 1.f);
        const bool afterCut = t >= cut;

        LayerRequest outgoingLayer;

        if (outgoing) {
            // Plays until the cut, then holds its last frame.
            outgoingLayer = makeLayer(*outgoing, i, outgoing == current ? 0 : 1,
                afterCut ? outgoing->duration - 1 : t - outgoing->start, afterCut);
        }

        // Holds its first frame until the cut, then plays.
        LayerRequest incomingLayer = makeLayer(*incoming, i, incoming == current ? 0 : 1,
            afterCut ? t - incoming->start : 0, !afterCut);

        applyTransition(incoming->transitionIn.type, p, (float)sequence.width, outgoing ? &outgoingLayer : nullptr, incomingLayer);

        if (outgoing) {
            layers.push_back(outgoingLayer);
        }

        layers.push_back(incomingLayer);
    }

    return layers;
}

}

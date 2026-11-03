#pragma once

#include <core/Time.h>

#include <vector>


namespace core
{
    // Curve of the segment that starts at a keyframe.
    enum class Easing
    {
        Linear,
        EaseIn,
        EaseOut,
        EaseInOut,
        Hold            // Keeps the value until the next keyframe
    };


    struct Keyframe
    {
        Time time = 0;      // From the start of the clip
        float value = 0.f;
        Easing easing = Easing::EaseInOut;
    };


    // A property of a clip that may vary over time: a constant value until
    // the first keyframe is added, then the keyframes, held before the first
    // and after the last one.
    struct AnimatedValue
    {
        float value = 0.f;
        std::vector<Keyframe> keys;     // Sorted by time

        AnimatedValue() = default;
        AnimatedValue(float constant) : value(constant) {}

        bool animated() const { return !keys.empty(); }

        float at(Time t) const;

        // Index of the keyframe at time t, or -1.
        int keyAt(Time t) const;

        // Sets the value at time t: the constant value when not animated,
        // otherwise the keyframe at t, created when there is none.
        void set(Time t, float newValue);

        // Adds a keyframe at t with the current value at t.
        void addKey(Time t, Easing easing = Easing::EaseInOut);
        void removeKey(Time t);

        // Removes the keyframes, keeping the value at t.
        void clearKeys(Time t);

        // Moves every keyframe by offset (after a trim of the clip start).
        void shiftKeys(Time offset);

        // Multiplies the value and those of the keyframes.
        void scaleValues(float factor);
    };


    // Eased progress in [0, 1] for linear progress x in [0, 1].
    float applyEasing(Easing easing, float x);
}

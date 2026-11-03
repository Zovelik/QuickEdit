#include <core/Animation.h>

#include <algorithm>
#include <cmath>


namespace core
{

float applyEasing(Easing easing, float x)
{
    x = std::clamp(x, 0.f, 1.f);

    switch (easing) {
        case Easing::EaseIn:    return x * x * x;
        case Easing::EaseOut:   return 1.f - std::pow(1.f - x, 3.f);
        case Easing::EaseInOut: return (x < 0.5f) ? 4.f * x * x * x : 1.f - std::pow(-2.f * x + 2.f, 3.f) / 2.f;
        case Easing::Hold:      return 0.f;
        default:                return x;
    }
}


float AnimatedValue::at(Time t) const
{
    if (keys.empty()) {
        return value;
    }

    if (t <= keys.front().time) {
        return keys.front().value;
    }

    if (t >= keys.back().time) {
        return keys.back().value;
    }

    // The segment holding t.
    const auto next = std::upper_bound(keys.begin(), keys.end(), t,
        [](Time time, const Keyframe& key) { return time < key.time; });
    const Keyframe& a = *(next - 1);
    const Keyframe& b = *next;

    const float x = (float)((double)(t - a.time) / (double)(b.time - a.time));

    return a.value + (b.value - a.value) * applyEasing(a.easing, x);
}


int AnimatedValue::keyAt(Time t) const
{
    for (size_t i = 0; i < keys.size(); i++) {
        if (keys[i].time == t) {
            return (int)i;
        }
    }

    return -1;
}


void AnimatedValue::set(Time t, float newValue)
{
    if (keys.empty()) {
        value = newValue;
        return;
    }

    const int index = keyAt(t);

    if (index >= 0) {
        keys[index].value = newValue;
    }
    else {
        addKey(t);
        keys[keyAt(t)].value = newValue;
    }
}


void AnimatedValue::addKey(Time t, Easing easing)
{
    if (keyAt(t) >= 0) {
        return;
    }

    const Keyframe key = { t, at(t), easing };
    keys.insert(std::upper_bound(keys.begin(), keys.end(), t,
        [](Time time, const Keyframe& k) { return time < k.time; }), key);
}


void AnimatedValue::removeKey(Time t)
{
    const int index = keyAt(t);

    if (index < 0) {
        return;
    }

    // The value stays the one of the last keyframe removed.
    if (keys.size() == 1) {
        value = keys[0].value;
    }

    keys.erase(keys.begin() + index);
}


void AnimatedValue::clearKeys(Time t)
{
    value = at(t);
    keys.clear();
}


void AnimatedValue::shiftKeys(Time offset)
{
    for (Keyframe& key : keys) {
        key.time += offset;
    }
}


void AnimatedValue::scaleValues(float factor)
{
    value *= factor;

    for (Keyframe& key : keys) {
        key.value *= factor;
    }
}

}

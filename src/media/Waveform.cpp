#include <media/AudioBuffer.h>

#include <algorithm>


namespace media
{

Waveform Waveform::build(const AudioBuffer& buffer)
{
    Waveform waveform;
    waveform.sampleRate = buffer.sampleRate;

    const int64_t frames = buffer.frameCount();

    if (frames == 0) {
        return waveform;
    }

    // Level 0, from the samples.
    std::vector<Peak> base((size_t)((frames + BaseBlockSize - 1) / BaseBlockSize));
    const float* samples = buffer.samples.data();

    for (size_t block = 0; block < base.size(); block++) {
        const int64_t first = (int64_t)block * BaseBlockSize;
        const int64_t last = std::min(frames, first + BaseBlockSize);

        float minValue = 1.f;
        float maxValue = -1.f;

        for (int64_t frame = first; frame < last; frame++) {
            const float value = 0.5f * (samples[2 * frame] + samples[2 * frame + 1]);
            minValue = std::min(minValue, value);
            maxValue = std::max(maxValue, value);
        }

        base[block] = { minValue, maxValue };
    }

    waveform.levels.push_back(std::move(base));

    // Next levels, from the previous one, down to a single peak.
    while (waveform.levels.back().size() > 1) {
        const std::vector<Peak>& previous = waveform.levels.back();
        std::vector<Peak> level((previous.size() + 1) / 2);

        for (size_t i = 0; i < level.size(); i++) {
            const Peak& a = previous[2 * i];
            const Peak& b = (2 * i + 1 < previous.size()) ? previous[2 * i + 1] : a;
            level[i] = { std::min(a.min, b.min), std::max(a.max, b.max) };
        }

        waveform.levels.push_back(std::move(level));
    }

    return waveform;
}

}

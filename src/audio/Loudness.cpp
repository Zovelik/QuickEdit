#include <audio/Loudness.h>

#include <algorithm>
#include <cmath>


namespace audio
{

LoudnessMeter::LoudnessMeter(uint32_t sampleRate)
{
    const double pi = 3.14159265358979323846;
    const double rate = (double)std::max<uint32_t>(sampleRate, 8000);

    // K-weighting, for any sample rate (the coefficients of BS.1770 are
    // those of 48 kHz): a high shelf of about +4 dB above 1.5 kHz...
    {
        const double f0 = 1681.974450955533;
        const double gain = 3.999843853973347;
        const double q = 0.7071752369554196;

        const double k = std::tan(pi * f0 / rate);
        const double vh = std::pow(10.0, gain / 20.0);
        const double vb = std::pow(vh, 0.4996667741545416);
        const double a0 = 1.0 + k / q + k * k;

        _shelf.b0 = (vh + vb * k / q + k * k) / a0;
        _shelf.b1 = 2.0 * (k * k - vh) / a0;
        _shelf.b2 = (vh - vb * k / q + k * k) / a0;
        _shelf.a1 = 2.0 * (k * k - 1.0) / a0;
        _shelf.a2 = (1.0 - k / q + k * k) / a0;
    }

    // ...then a high-pass at 38 Hz.
    {
        const double f0 = 38.13547087602444;
        const double q = 0.5003270373238773;

        const double k = std::tan(pi * f0 / rate);
        const double a0 = 1.0 + k / q + k * k;

        _highPass.b0 = 1.0;
        _highPass.b1 = -2.0;
        _highPass.b2 = 1.0;
        _highPass.a1 = 2.0 * (k * k - 1.0) / a0;
        _highPass.a2 = (1.0 - k / q + k * k) / a0;
    }

    _partFrames = std::max<int64_t>(1, (int64_t)std::llround(rate / 10.0));
}


void LoudnessMeter::add(const float* samples, int64_t frames)
{
    for (int64_t i = 0; i < frames; i++) {
        for (int channel = 0; channel < 2; channel++) {
            const float sample = samples[2 * i + channel];
            _peak = std::max(_peak, std::abs(sample));

            const double weighted = _highPass.process(_shelf.process((double)sample, channel), channel);
            _partEnergy += weighted * weighted;
        }

        if (++_framesInPart == _partFrames) {
            _parts.push_back(_partEnergy);
            _partEnergy = 0.0;
            _framesInPart = 0;
        }
    }
}


double LoudnessMeter::integratedLufs() const
{
    // Blocks of four parts (400 ms), one every part (75 % overlap): their
    // mean power and loudness.
    std::vector<double> powers;

    for (size_t i = 3; i < _parts.size(); i++) {
        const double energy = _parts[i - 3] + _parts[i - 2] + _parts[i - 1] + _parts[i];
        powers.push_back(energy / (double)(4 * _partFrames));
    }

    auto loudness = [](double power) { return -0.691 + 10.0 * std::log10(std::max(power, 1e-20)); };

    auto gatedMean = [&](double gate, double& mean) {
        double sum = 0.0;
        size_t count = 0;

        for (double power : powers) {
            if (loudness(power) > gate) {
                sum += power;
                count++;
            }
        }

        mean = count ? sum / (double)count : 0.0;
        return count > 0;
    };

    double mean = 0.0;

    if (!gatedMean(Silence, mean)) {
        return Silence - 1.0;
    }

    const double relativeGate = loudness(mean) - 10.0;

    if (!gatedMean(std::max(Silence, relativeGate), mean)) {
        return Silence - 1.0;
    }

    return loudness(mean);
}


double normalizationGainDb(double lufs, float peak, double targetLufs, double ceilingDb)
{
    if (lufs < LoudnessMeter::Silence) {
        return 0.0;
    }

    double gain = targetLufs - lufs;

    // The peak under the ceiling.
    if (peak > 0.f) {
        const double peakDb = 20.0 * std::log10((double)peak);
        gain = std::min(gain, ceilingDb - peakDb);
    }

    return gain;
}

}

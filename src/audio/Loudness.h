#pragma once

#include <cstdint>
#include <vector>


namespace audio
{
    // Integrated loudness of a stereo signal (ITU-R BS.1770-4, EBU R128), in
    // LUFS: K-weighting, 400 ms blocks every 100 ms, absolute gate at
    // -70 LUFS and relative gate 10 LU below. Also the sample peak.
    class LoudnessMeter
    {
    public:
        explicit LoudnessMeter(uint32_t sampleRate);

        // Interleaved stereo frames, in order.
        void add(const float* samples, int64_t frames);

        // -infinity (a very low value) for silence.
        double integratedLufs() const;

        // Largest absolute sample, linear.
        float peak() const { return _peak; }

        // Silence reads below this.
        static constexpr double Silence = -70.0;

    private:
        struct Biquad
        {
            double b0, b1, b2, a1, a2;
            double z1[2] = { 0.0, 0.0 };
            double z2[2] = { 0.0, 0.0 };

            double process(double x, int channel)
            {
                const double y = b0 * x + z1[channel];
                z1[channel] = b1 * x - a1 * y + z2[channel];
                z2[channel] = b2 * x - a2 * y;
                return y;
            }
        };

        Biquad _shelf;
        Biquad _highPass;

        // Energy of the 100 ms parts, and of the part being filled.
        int64_t _partFrames;
        int64_t _framesInPart = 0;
        double _partEnergy = 0.0;
        std::vector<double> _parts;
        float _peak = 0.f;
    };


    // Gain in dB bringing a signal of the given loudness and peak to the
    // target loudness, lowered so that the peak stays under ceilingDb.
    double normalizationGainDb(double lufs, float peak, double targetLufs, double ceilingDb = -1.0);
}

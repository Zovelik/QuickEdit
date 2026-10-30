#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>


namespace core
{
    // Times are counted in ticks of 1/705600000 s (a "flick"). This unit
    // divides exactly every common frame rate (24, 25, 30, 48, 50, 60, 90,
    // 100, 120 fps) and audio sample rate (44.1, 48, 88.2, 96, 192 kHz), so
    // frame and sample boundaries never accumulate rounding errors.
    //
    // NTSC rates (30000/1001...) are not exact in ticks: they go through
    // Rational, see frameToTime() and timeToFrame().
    using Time = int64_t;

    constexpr Time TicksPerSecond = 705600000;


    struct Rational
    {
        int64_t num = 0;
        int64_t den = 1;

        double toDouble() const { return den != 0 ? (double)num / (double)den : 0.0; }

        bool operator==(const Rational& other) const { return num * other.den == other.num * den; }
    };


    inline double timeToSeconds(Time t)
    {
        return (double)t / (double)TicksPerSecond;
    }

    inline Time secondsToTime(double seconds)
    {
        return (Time)std::llround(seconds * (double)TicksPerSecond);
    }

    // Floor of a * b / c for c > 0, without overflowing the intermediate
    // product as long as (a % c) * b fits in 64 bits (always the case for
    // frame rates and timeline lengths, which are far from the limits).
    inline int64_t mulDivFloor(int64_t a, int64_t b, int64_t c)
    {
        int64_t q = a / c;
        int64_t r = a % c;

        // Floor division, also for negative values.
        if (r < 0) {
            q -= 1;
            r += c;
        }

        return q * b + (r * b) / c;
    }

    // Start of the given frame, for a frame rate of fps (frames per second).
    inline Time frameToTime(int64_t frame, const Rational& fps)
    {
        // t = frame * den / num seconds, rounded up: for the NTSC rates, the
        // start of a frame is then never before its exact time, so that
        // timeToFrame() finds the frame back.
        return -mulDivFloor(-frame, fps.den * TicksPerSecond, fps.num);
    }

    // Frame shown at time t (the frame whose start is at or before t).
    inline int64_t timeToFrame(Time t, const Rational& fps)
    {
        return mulDivFloor(t, fps.num, fps.den * TicksPerSecond);
    }

    // Snaps t to the start of its frame.
    inline Time snapToFrame(Time t, const Rational& fps)
    {
        return frameToTime(timeToFrame(t, fps), fps);
    }

    // Formats t as HH:MM:SS:FF for the given frame rate (non drop-frame).
    inline std::string formatTimecode(Time t, const Rational& fps)
    {
        const bool negative = t < 0;
        const int64_t frame = timeToFrame(negative ? -t : t, fps);
        const int64_t fpsRounded = std::max<int64_t>(1, std::llround(fps.toDouble()));

        const int64_t ff = frame % fpsRounded;
        const int64_t totalSeconds = frame / fpsRounded;
        const int64_t ss = totalSeconds % 60;
        const int64_t mm = (totalSeconds / 60) % 60;
        const int64_t hh = totalSeconds / 3600;

        char buffer[96];        // Any int64 fits
        std::snprintf(buffer, sizeof(buffer), "%s%02lld:%02lld:%02lld:%02lld",
            negative ? "-" : "", (long long)hh, (long long)mm, (long long)ss, (long long)ff);

        return buffer;
    }
}

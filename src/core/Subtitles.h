#pragma once

#include <core/Project.h>

#include <string>
#include <vector>


namespace core
{
    // A line of subtitles, from start to end in the sequence.
    struct SubtitleCue
    {
        Time start = 0;
        Time end = 0;
        std::string text;               // UTF-8, '\n' between lines
    };

    // The cues of the shown subtitle tracks, in time order.
    std::vector<SubtitleCue> subtitleCues(const Sequence& sequence);

    // SubRip (.srt) text of cues, and cues of SubRip text (the entries it
    // cannot read are skipped).
    std::string toSrt(const std::vector<SubtitleCue>& cues);
    std::vector<SubtitleCue> parseSrt(const std::string& text);

    // The subtitles shown at t, as text clips in the style of the sequence,
    // placed at the bottom of the frame (to draw over the other layers).
    std::vector<Clip> subtitleClipsAt(const Sequence& sequence, Time t);
}

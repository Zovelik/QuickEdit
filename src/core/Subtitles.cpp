#include <core/Subtitles.h>

#include <algorithm>
#include <cstdio>
#include <sstream>


namespace core
{

namespace {

std::string srtTime(Time t)
{
    const int64_t ms = std::max<int64_t>(0, (int64_t)(timeToSeconds(t) * 1000.0 + 0.5));
    char text[32];
    std::snprintf(text, sizeof(text), "%02lld:%02lld:%02lld,%03lld",
        (long long)(ms / 3600000), (long long)(ms / 60000 % 60), (long long)(ms / 1000 % 60), (long long)(ms % 1000));
    return text;
}


// "hh:mm:ss,mmm" (or a dot); false when it is not a time.
bool parseSrtTime(const std::string& text, Time& t)
{
    int h = 0, m = 0, s = 0, ms = 0;
    char separator = 0;

    if (std::sscanf(text.c_str(), "%d:%d:%d%c%d", &h, &m, &s, &separator, &ms) != 5) {
        return false;
    }

    t = secondsToTime(h * 3600.0 + m * 60.0 + s + ms / 1000.0);
    return true;
}


std::string trimmed(const std::string& line)
{
    const size_t first = line.find_first_not_of(" \t\r\n\xef\xbb\xbf");
    const size_t last = line.find_last_not_of(" \t\r\n");
    return (first == std::string::npos) ? std::string() : line.substr(first, last - first + 1);
}

} // namespace


std::vector<SubtitleCue> subtitleCues(const Sequence& sequence)
{
    std::vector<SubtitleCue> cues;

    for (const Track& track : sequence.subtitleTracks) {
        if (track.muted) {
            continue;
        }

        for (const Clip& clip : track.clips) {
            cues.push_back({ clip.start, clip.end(), clip.text.plainText() });
        }
    }

    std::sort(cues.begin(), cues.end(), [](const SubtitleCue& a, const SubtitleCue& b) { return a.start < b.start; });

    return cues;
}


std::string toSrt(const std::vector<SubtitleCue>& cues)
{
    std::string text;
    int index = 1;

    for (const SubtitleCue& cue : cues) {
        if (trimmed(cue.text).empty()) {
            continue;
        }

        text += std::to_string(index++) + "\r\n";
        text += srtTime(cue.start) + " --> " + srtTime(cue.end) + "\r\n";

        std::string body = cue.text;
        size_t at = 0;

        while ((at = body.find('\n', at)) != std::string::npos) {
            body.replace(at, 1, "\r\n");
            at += 2;
        }

        text += body + "\r\n\r\n";
    }

    return text;
}


std::vector<SubtitleCue> parseSrt(const std::string& text)
{
    std::vector<SubtitleCue> cues;
    std::istringstream lines(text);
    std::string line;
    SubtitleCue cue;
    bool inCue = false;

    auto finish = [&]() {
        if (inCue && cue.end > cue.start) {
            while (!cue.text.empty() && cue.text.back() == '\n') cue.text.pop_back();
            cues.push_back(cue);
        }

        cue = SubtitleCue();
        inCue = false;
    };

    while (std::getline(lines, line)) {
        const std::string content = trimmed(line);
        const size_t arrow = content.find("-->");

        if (arrow != std::string::npos) {
            finish();

            Time start = 0, end = 0;

            if (parseSrtTime(trimmed(content.substr(0, arrow)), start) && parseSrtTime(trimmed(content.substr(arrow + 3)), end)) {
                cue.start = start;
                cue.end = end;
                inCue = true;
            }
        }
        else if (content.empty()) {
            finish();
        }
        else if (inCue) {
            cue.text += (cue.text.empty() ? "" : "\n") + content;
        }
    }

    finish();

    return cues;
}


std::vector<Clip> subtitleClipsAt(const Sequence& sequence, Time t)
{
    std::vector<Clip> clips;
    const SubtitleStyle& style = sequence.subtitleStyle;

    for (const Track& track : sequence.subtitleTracks) {
        if (track.muted) {
            continue;
        }

        const int index = track.clipAt(t);

        if (index < 0) {
            continue;
        }

        const Clip& cue = track.clips[index];
        const std::string text = cue.text.plainText();

        if (text.empty()) {
            continue;
        }

        Clip clip;
        clip.id = cue.id;
        clip.kind = ClipKind::Text;
        clip.start = cue.start;
        clip.duration = cue.duration;

        TextStyle textStyle;
        textStyle.size = style.size;
        textStyle.color = style.color;
        textStyle.outlineWidth = style.outlineWidth;
        textStyle.outlineColor = { 0.f, 0.f, 0.f, 1.f };
        textStyle.shadowOffsetY = 2.f;
        textStyle.shadowSoftness = 3.f;

        clip.text.spans.push_back({ text, textStyle });
        clip.text.align = TextAlign::Center;
        clip.text.boxWidth = 0.9f * (float)sequence.width;

        // The block centered on its middle: its bottom at the margin.
        const int lines = 1 + (int)std::count(text.begin(), text.end(), '\n');
        clip.transform.y = 0.5f * (float)sequence.height - style.bottomMargin - 0.6f * style.size * (float)lines;

        clips.push_back(std::move(clip));
    }

    return clips;
}

}

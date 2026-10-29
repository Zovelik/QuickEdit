#include <core/Project.h>

#include <algorithm>
#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>


namespace core
{

namespace {

// Durations given to the media before they are probed, and to still images.
constexpr Time DefaultUnprobedDuration = 10 * TicksPerSecond;

constexpr Time MaxTime = std::numeric_limits<Time>::max() / 4;


const char* toString(MediaType type)
{
    switch (type) {
        case MediaType::Audio: return "audio";
        case MediaType::Image: return "image";
        default:               return "video";
    }
}


MediaType mediaTypeFromString(const std::string& value)
{
    if (value == "audio") return MediaType::Audio;
    if (value == "image") return MediaType::Image;
    return MediaType::Video;
}


std::string lowercase(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return value;
}


Json rationalToJson(const Rational& value)
{
    Json json = Json::array();
    json.push_back(value.num);
    json.push_back(value.den);
    return json;
}


Rational rationalFromJson(const Json& json, const Rational& defaultValue)
{
    if (!json.isArray() || json.size() != 2 || json[1].toInt() <= 0 || json[0].toInt() <= 0) {
        return defaultValue;
    }

    return { json[0].toInt(), json[1].toInt() };
}


// Paths are stored relative to the project file when possible, so that a
// project folder can be moved with its media.
std::string pathToJson(const std::filesystem::path& path, const std::filesystem::path& projectDir)
{
    std::filesystem::path stored = path;

    if (!projectDir.empty()) {
        std::error_code error;
        const std::filesystem::path relative = std::filesystem::relative(path, projectDir, error);

        if (!error && !relative.empty() && *relative.begin() != "..") {
            stored = relative;
        }
    }

    const std::u8string u8 = stored.generic_u8string();
    return std::string(u8.begin(), u8.end());
}


std::filesystem::path pathFromJson(const std::string& value, const std::filesystem::path& projectDir)
{
    const std::filesystem::path path(std::u8string(value.begin(), value.end()));

    if (path.is_relative() && !projectDir.empty()) {
        return (projectDir / path).lexically_normal();
    }

    return path;
}


// An animated value is a number while it has no keyframe, otherwise
// { "value": v, "keys": [[time, value, easing], ...] }.
Json animatedToJson(const AnimatedValue& value)
{
    if (!value.animated()) {
        return Json((double)value.value);
    }

    Json json = Json::object();
    json["value"] = (double)value.value;

    Json keys = Json::array();

    for (const Keyframe& key : value.keys) {
        Json entry = Json::array();
        entry.push_back(key.time);
        entry.push_back((double)key.value);
        entry.push_back((int)key.easing);
        keys.push_back(entry);
    }

    json["keys"] = keys;
    return json;
}


AnimatedValue animatedFromJson(const Json& json, float defaultValue)
{
    AnimatedValue value(defaultValue);

    if (json.isNumber()) {
        value.value = (float)json.toDouble();
        return value;
    }

    value.value = (float)json["value"].toDouble(defaultValue);

    for (const Json& entry : json["keys"].items()) {
        Keyframe key;
        key.time = entry[0].toInt();
        key.value = (float)entry[1].toDouble();
        key.easing = (Easing)std::clamp<int64_t>(entry[2].toInt(), 0, (int64_t)Easing::Hold);
        value.keys.push_back(key);
    }

    std::sort(value.keys.begin(), value.keys.end(), [](const Keyframe& a, const Keyframe& b) { return a.time < b.time; });

    return value;
}


Json colorToJson(const Color& color)
{
    Json json = Json::array();
    json.push_back((double)color.r);
    json.push_back((double)color.g);
    json.push_back((double)color.b);
    json.push_back((double)color.a);
    return json;
}


Color colorFromJson(const Json& json, const Color& defaultValue)
{
    if (!json.isArray() || json.size() != 4) {
        return defaultValue;
    }

    return { (float)json[0].toDouble(), (float)json[1].toDouble(), (float)json[2].toDouble(), (float)json[3].toDouble() };
}


Json styleToJson(const TextStyle& style)
{
    Json json = Json::object();
    json["font"] = style.font;
    json["size"] = (double)style.size;
    json["color"] = colorToJson(style.color);
    json["letterSpacing"] = (double)style.letterSpacing;
    json["outlineWidth"] = (double)style.outlineWidth;
    json["outlineColor"] = colorToJson(style.outlineColor);
    json["shadowOffsetX"] = (double)style.shadowOffsetX;
    json["shadowOffsetY"] = (double)style.shadowOffsetY;
    json["shadowSoftness"] = (double)style.shadowSoftness;
    json["shadowColor"] = colorToJson(style.shadowColor);
    return json;
}


TextStyle styleFromJson(const Json& json)
{
    const TextStyle defaults;
    TextStyle style;
    style.font = json["font"].toString();
    style.size = (float)std::max(1.0, json["size"].toDouble(defaults.size));
    style.color = colorFromJson(json["color"], defaults.color);
    style.letterSpacing = (float)json["letterSpacing"].toDouble(0.0);
    style.outlineWidth = (float)json["outlineWidth"].toDouble(0.0);
    style.outlineColor = colorFromJson(json["outlineColor"], defaults.outlineColor);
    style.shadowOffsetX = (float)json["shadowOffsetX"].toDouble(0.0);
    style.shadowOffsetY = (float)json["shadowOffsetY"].toDouble(0.0);
    style.shadowSoftness = (float)json["shadowSoftness"].toDouble(0.0);
    style.shadowColor = colorFromJson(json["shadowColor"], defaults.shadowColor);
    return style;
}


Json textToJson(const TextContent& text)
{
    Json json = Json::object();
    json["align"] = (int)text.align;
    json["lineSpacing"] = (double)text.lineSpacing;
    json["boxWidth"] = (double)text.boxWidth;

    Json animation = Json::object();
    animation["in"] = (int)text.animation.in;
    animation["out"] = (int)text.animation.out;
    animation["letterDelay"] = text.animation.letterDelay;
    animation["letterDuration"] = text.animation.letterDuration;
    json["animation"] = animation;

    Json spans = Json::array();

    for (const TextSpan& span : text.spans) {
        Json entry = Json::object();
        entry["text"] = span.text;
        entry["style"] = styleToJson(span.style);
        spans.push_back(entry);
    }

    json["spans"] = spans;
    return json;
}


TextContent textFromJson(const Json& json)
{
    const TextContent defaults;
    TextContent text;
    text.align = (TextAlign)std::clamp<int64_t>(json["align"].toInt((int)defaults.align), 0, (int64_t)TextAlign::Right);
    text.lineSpacing = (float)json["lineSpacing"].toDouble(defaults.lineSpacing);
    text.boxWidth = (float)std::max(0.0, json["boxWidth"].toDouble(0.0));

    const Json& animation = json["animation"];
    text.animation.in = (LetterAnimation)std::clamp<int64_t>(animation["in"].toInt(), 0, (int64_t)LetterAnimation::Pop);
    text.animation.out = (LetterAnimation)std::clamp<int64_t>(animation["out"].toInt(), 0, (int64_t)LetterAnimation::Pop);
    text.animation.letterDelay = std::max<Time>(0, animation["letterDelay"].toInt(defaults.animation.letterDelay));
    text.animation.letterDuration = std::max<Time>(1, animation["letterDuration"].toInt(defaults.animation.letterDuration));

    for (const Json& entry : json["spans"].items()) {
        text.spans.push_back({ entry["text"].toString(), styleFromJson(entry["style"]) });
    }

    return text;
}


Json clipToJson(const Clip& clip)
{
    Json json = Json::object();
    json["id"] = clip.id;
    json["kind"] = (clip.kind == ClipKind::Text) ? "text" : "media";
    json["media"] = clip.mediaId;
    json["start"] = clip.start;
    json["duration"] = clip.duration;
    json["sourceIn"] = clip.sourceIn;
    json["link"] = clip.linkGroup;
    json["gainDb"] = (double)clip.gainDb;
    json["frozen"] = clip.frozen;

    if (clip.fadeIn > 0) json["fadeIn"] = clip.fadeIn;
    if (clip.fadeOut > 0) json["fadeOut"] = clip.fadeOut;

    Json transform = Json::object();
    transform["x"] = animatedToJson(clip.transform.x);
    transform["y"] = animatedToJson(clip.transform.y);
    transform["scale"] = animatedToJson(clip.transform.scale);
    transform["rotation"] = animatedToJson(clip.transform.rotation);
    transform["opacity"] = animatedToJson(clip.transform.opacity);
    json["transform"] = transform;

    if (clip.kind == ClipKind::Text) {
        json["text"] = textToJson(clip.text);
    }

    if (clip.frame != Frame()) {
        const Frame& f = clip.frame;
        Json frame = Json::object();
        frame["cropLeft"] = (double)f.cropLeft;
        frame["cropRight"] = (double)f.cropRight;
        frame["cropTop"] = (double)f.cropTop;
        frame["cropBottom"] = (double)f.cropBottom;
        frame["cornerRadius"] = (double)f.cornerRadius;
        frame["borderWidth"] = (double)f.borderWidth;
        frame["borderColor"] = colorToJson(f.borderColor);
        frame["shadowOffsetX"] = (double)f.shadowOffsetX;
        frame["shadowOffsetY"] = (double)f.shadowOffsetY;
        frame["shadowSoftness"] = (double)f.shadowSoftness;
        frame["shadowColor"] = colorToJson(f.shadowColor);
        json["frame"] = frame;
    }

    if (clip.transitionIn.type != TransitionType::None) {
        Json transition = Json::object();
        transition["type"] = (int)clip.transitionIn.type;
        transition["duration"] = clip.transitionIn.duration;
        json["transitionIn"] = transition;
    }

    return json;
}


Clip clipFromJson(const Json& json)
{
    Clip clip;
    clip.id = (Id)json["id"].toInt();
    clip.kind = (json["kind"].toString() == "text") ? ClipKind::Text : ClipKind::Media;
    clip.mediaId = (Id)json["media"].toInt();
    clip.start = json["start"].toInt();
    clip.duration = std::max<Time>(1, json["duration"].toInt(1));
    clip.sourceIn = json["sourceIn"].toInt();
    clip.linkGroup = (Id)json["link"].toInt();
    clip.gainDb = (float)json["gainDb"].toDouble(0.0);
    clip.frozen = json["frozen"].toBool();
    clip.fadeIn = std::clamp<Time>(json["fadeIn"].toInt(), 0, clip.duration);
    clip.fadeOut = std::clamp<Time>(json["fadeOut"].toInt(), 0, clip.duration);

    const Json& transform = json["transform"];
    clip.transform.x = animatedFromJson(transform["x"], 0.f);
    clip.transform.y = animatedFromJson(transform["y"], 0.f);
    clip.transform.scale = animatedFromJson(transform["scale"], 1.f);
    clip.transform.rotation = animatedFromJson(transform["rotation"], 0.f);
    clip.transform.opacity = animatedFromJson(transform["opacity"], (float)json["opacity"].toDouble(1.0));

    if (clip.kind == ClipKind::Text) {
        clip.text = textFromJson(json["text"]);
    }

    if (const Json& frame = json["frame"]; frame.isObject()) {
        const Frame defaults;
        Frame& f = clip.frame;
        f.cropLeft = std::clamp((float)frame["cropLeft"].toDouble(0.0), 0.f, 1.f);
        f.cropRight = std::clamp((float)frame["cropRight"].toDouble(0.0), 0.f, 1.f);
        f.cropTop = std::clamp((float)frame["cropTop"].toDouble(0.0), 0.f, 1.f);
        f.cropBottom = std::clamp((float)frame["cropBottom"].toDouble(0.0), 0.f, 1.f);
        f.cornerRadius = std::max(0.f, (float)frame["cornerRadius"].toDouble(0.0));
        f.borderWidth = std::max(0.f, (float)frame["borderWidth"].toDouble(0.0));
        f.borderColor = colorFromJson(frame["borderColor"], defaults.borderColor);
        f.shadowOffsetX = (float)frame["shadowOffsetX"].toDouble(0.0);
        f.shadowOffsetY = (float)frame["shadowOffsetY"].toDouble(0.0);
        f.shadowSoftness = std::max(0.f, (float)frame["shadowSoftness"].toDouble(0.0));
        f.shadowColor = colorFromJson(frame["shadowColor"], defaults.shadowColor);
    }

    const Json& transition = json["transitionIn"];
    clip.transitionIn.type = (TransitionType)std::clamp<int64_t>(transition["type"].toInt(), 0, (int64_t)TransitionType::Zoom);
    clip.transitionIn.duration = std::max<Time>(1, transition["duration"].toInt(TicksPerSecond));

    return clip;
}


Json trackToJson(const Track& track)
{
    Json json = Json::object();
    json["id"] = track.id;
    json["name"] = track.name;
    json["muted"] = track.muted;
    json["solo"] = track.solo;
    json["locked"] = track.locked;

    Json clips = Json::array();

    for (const Clip& clip : track.clips) {
        clips.push_back(clipToJson(clip));
    }

    json["clips"] = clips;

    if (!track.effects.empty()) {
        Json effects = Json::array();

        for (const EffectSlot& slot : track.effects) {
            Json entry = Json::object();
            entry["id"] = slot.id;
            entry["path"] = slot.path;
            entry["classId"] = slot.classId;
            entry["name"] = slot.name;
            entry["bypass"] = slot.bypass;
            entry["state"] = slot.state;
            effects.push_back(entry);
        }

        json["effects"] = effects;
    }

    return json;
}


Track trackFromJson(const Json& json, TrackType type)
{
    Track track;
    track.id = (Id)json["id"].toInt();
    track.type = type;
    track.name = json["name"].toString();
    track.muted = json["muted"].toBool();
    track.solo = json["solo"].toBool();
    track.locked = json["locked"].toBool();

    for (const Json& clip : json["clips"].items()) {
        track.clips.push_back(clipFromJson(clip));
    }

    track.sortClips();

    for (const Json& entry : json["effects"].items()) {
        EffectSlot slot;
        slot.id = (Id)entry["id"].toInt();
        slot.path = entry["path"].toString();
        slot.classId = entry["classId"].toString();
        slot.name = entry["name"].toString();
        slot.bypass = entry["bypass"].toBool();
        slot.state = entry["state"].toString();
        track.effects.push_back(slot);
    }

    return track;
}

} // namespace


MediaType mediaTypeFromExtension(const std::filesystem::path& path)
{
    const std::string ext = lowercase(path.extension().string());

    static const char* const audioExtensions[] = { ".wav", ".mp3", ".flac", ".ogg", ".opus", ".aac", ".m4a", ".aiff", ".aif" };
    static const char* const imageExtensions[] = { ".png", ".jpg", ".jpeg", ".bmp", ".tga", ".tif", ".tiff", ".exr", ".webp" };

    for (const char* audio : audioExtensions) {
        if (ext == audio) return MediaType::Audio;
    }

    for (const char* image : imageExtensions) {
        if (ext == image) return MediaType::Image;
    }

    return MediaType::Video;
}


std::string MediaItem::name() const
{
    const std::u8string u8 = path.filename().u8string();
    return std::string(u8.begin(), u8.end());
}


void Transform::shiftKeys(Time offset)
{
    for (AnimatedValue* value : { &x, &y, &scale, &rotation, &opacity }) {
        value->shiftKeys(offset);
    }
}


int Track::clipAt(Time t) const
{
    for (size_t i = 0; i < clips.size(); i++) {
        if (t >= clips[i].start && t < clips[i].end()) {
            return (int)i;
        }
    }

    return -1;
}


void Track::sortClips()
{
    std::sort(clips.begin(), clips.end(), [](const Clip& a, const Clip& b) { return a.start < b.start; });
}


Time Sequence::duration() const
{
    Time end = 0;

    for (const auto* tracks : { &videoTracks, &soundTracks, &audioTracks }) {
        for (const Track& track : *tracks) {
            for (const Clip& clip : track.clips) {
                end = std::max(end, clip.end());
            }
        }
    }

    return end;
}


void Sequence::addTrack(TrackType type, Id id, Id soundId)
{
    Track track;
    track.id = id;
    track.type = (type == TrackType::Video) ? TrackType::Video : TrackType::Audio;

    if (track.type == TrackType::Video) {
        track.name = "V" + std::to_string(videoTracks.size() + 1);
        videoTracks.push_back(track);

        Track sound;
        sound.id = soundId;
        sound.type = TrackType::Sound;
        sound.name = "S" + std::to_string(soundTracks.size() + 1);
        soundTracks.push_back(sound);
    }
    else {
        track.name = "A" + std::to_string(audioTracks.size() + 1);
        audioTracks.push_back(track);
    }
}


Project::Project()
{
    // Default sequence: three video tracks with their sound tracks, and two
    // audio tracks (voice, music).
    for (int i = 0; i < 3; i++) {
        const Id video = newId();
        _sequence.addTrack(TrackType::Video, video, newId());
    }

    for (int i = 0; i < 2; i++) {
        _sequence.addTrack(TrackType::Audio, newId(), InvalidId);
    }
}


MediaItem* Project::findMedia(Id id)
{
    for (MediaItem& item : _media) {
        if (item.id == id) return &item;
    }

    return nullptr;
}


const MediaItem* Project::findMedia(Id id) const
{
    return const_cast<Project*>(this)->findMedia(id);
}


Track* Project::findTrackOfClip(Id clipId)
{
    for (auto* tracks : { &_sequence.videoTracks, &_sequence.soundTracks, &_sequence.audioTracks }) {
        for (Track& track : *tracks) {
            for (const Clip& clip : track.clips) {
                if (clip.id == clipId) return &track;
            }
        }
    }

    return nullptr;
}


Clip* Project::findClip(Id clipId)
{
    Track* track = findTrackOfClip(clipId);

    if (!track) {
        return nullptr;
    }

    for (Clip& clip : track->clips) {
        if (clip.id == clipId) return &clip;
    }

    return nullptr;
}


Track* Project::findTrack(Id trackId)
{
    for (auto* tracks : { &_sequence.videoTracks, &_sequence.soundTracks, &_sequence.audioTracks }) {
        for (Track& track : *tracks) {
            if (track.id == trackId) return &track;
        }
    }

    return nullptr;
}


EffectSlot* Project::findEffect(Id slotId)
{
    for (Track& track : _sequence.soundTracks) {
        for (EffectSlot& slot : track.effects) {
            if (slot.id == slotId) return &slot;
        }
    }

    for (Track& track : _sequence.audioTracks) {
        for (EffectSlot& slot : track.effects) {
            if (slot.id == slotId) return &slot;
        }
    }

    return nullptr;
}


Id Project::importMedia(const std::filesystem::path& path)
{
    const std::filesystem::path normalized = path.lexically_normal();

    for (const MediaItem& item : _media) {
        if (item.path == normalized) {
            return item.id;
        }
    }

    MediaItem item;
    item.id = newId();
    item.path = normalized;
    item.type = mediaTypeFromExtension(normalized);

    // Placeholders until the decoder probes the file.
    item.duration = (item.type == MediaType::Image) ? DefaultImageDuration : DefaultUnprobedDuration;
    item.hasAudio = item.type != MediaType::Image;

    if (item.type != MediaType::Audio) {
        item.width = _sequence.width;
        item.height = _sequence.height;
        item.frameRate = _sequence.frameRate;
    }

    _media.push_back(item);

    return item.id;
}


bool Project::freeRangeAt(const Track& track, Time t, Id ignoreClip, Time& freeStart, Time& freeEnd) const
{
    freeStart = 0;
    freeEnd = MaxTime;

    for (const Clip& clip : track.clips) {
        if (clip.id == ignoreClip) {
            continue;
        }

        if (t >= clip.start && t < clip.end()) {
            return false;
        }

        if (clip.end() <= t) {
            freeStart = std::max(freeStart, clip.end());
        }
        else {
            freeEnd = std::min(freeEnd, clip.start);
        }
    }

    return true;
}


Id Project::addClip(Id mediaId, Id trackId, Time start)
{
    const MediaItem* media = findMedia(mediaId);
    Track* target = findTrack(trackId);

    if (!media || !target || target->locked || start < 0) {
        return InvalidId;
    }

    const bool wantsVideo = media->type != MediaType::Audio;
    const bool wantsAudio = media->type == MediaType::Audio || media->hasAudio;

    // A video goes on the target video track (or the one of the target
    // sound track, the first one otherwise), its sound on the sound track
    // paired with it. A sound file goes on the target audio track, the first
    // one otherwise.
    Track* videoTrack = nullptr;
    Track* audioTrack = nullptr;

    auto indexOf = [&](const std::vector<Track>& tracks) {
        for (size_t i = 0; i < tracks.size(); i++) {
            if (&tracks[i] == target) return (int)i;
        }
        return -1;
    };

    if (wantsVideo) {
        int index = indexOf(_sequence.videoTracks);

        if (index < 0) index = indexOf(_sequence.soundTracks);
        if (index < 0) index = 0;

        if (index < (int)_sequence.videoTracks.size()) {
            videoTrack = &_sequence.videoTracks[index];

            if (wantsAudio && index < (int)_sequence.soundTracks.size()) {
                audioTrack = &_sequence.soundTracks[index];
            }
        }
    }
    else if (wantsAudio) {
        audioTrack = (target->type == TrackType::Audio) ? target :
            (_sequence.audioTracks.empty() ? nullptr : &_sequence.audioTracks.front());
    }

    // Locked tracks get nothing.
    if (videoTrack && videoTrack->locked) videoTrack = nullptr;
    if (audioTrack && audioTrack->locked) audioTrack = nullptr;

    if (!videoTrack && !audioTrack) {
        return InvalidId;
    }

    // The clips are shortened to fit before the next clip of either track.
    Time duration = media->duration;

    for (Track* track : { videoTrack, audioTrack }) {
        if (!track) {
            continue;
        }

        Time freeStart, freeEnd;

        if (!freeRangeAt(*track, start, InvalidId, freeStart, freeEnd)) {
            return InvalidId;
        }

        duration = std::min(duration, freeEnd - start);
    }

    if (duration <= 0) {
        return InvalidId;
    }

    const Id linkGroup = (videoTrack && audioTrack) ? newId() : InvalidId;
    Id firstClipId = InvalidId;

    for (Track* track : { videoTrack, audioTrack }) {
        if (!track) {
            continue;
        }

        Clip clip;
        clip.id = newId();
        clip.mediaId = mediaId;
        clip.start = start;
        clip.duration = duration;
        clip.sourceIn = 0;
        clip.linkGroup = linkGroup;

        track->clips.push_back(clip);
        track->sortClips();

        if (firstClipId == InvalidId) {
            firstClipId = clip.id;
        }
    }

    return firstClipId;
}


void Project::removeClips(const std::vector<Id>& clipIds, bool withLinks)
{
    // Linked clips go with the selected ones.
    std::vector<Id> linkGroups;

    for (Id clipId : withLinks ? clipIds : std::vector<Id>()) {
        const Clip* clip = findClip(clipId);

        if (clip && clip->linkGroup != InvalidId) {
            linkGroups.push_back(clip->linkGroup);
        }
    }

    for (auto* tracks : { &_sequence.videoTracks, &_sequence.soundTracks, &_sequence.audioTracks }) {
        for (Track& track : *tracks) {
            if (track.locked) {
                continue;
            }

            std::erase_if(track.clips, [&](const Clip& clip) {
                return std::find(clipIds.begin(), clipIds.end(), clip.id) != clipIds.end() ||
                    (clip.linkGroup != InvalidId && std::find(linkGroups.begin(), linkGroups.end(), clip.linkGroup) != linkGroups.end());
            });
        }
    }
}


void Project::rippleDelete(const std::vector<Id>& clipIds, bool withLinks)
{
    std::vector<Id> linkGroups;

    for (Id clipId : withLinks ? clipIds : std::vector<Id>()) {
        const Clip* clip = findClip(clipId);

        if (clip && clip->linkGroup != InvalidId) {
            linkGroups.push_back(clip->linkGroup);
        }
    }

    auto removed = [&](const Clip& clip) {
        return std::find(clipIds.begin(), clipIds.end(), clip.id) != clipIds.end() ||
            (clip.linkGroup != InvalidId && std::find(linkGroups.begin(), linkGroups.end(), clip.linkGroup) != linkGroups.end());
    };

    for (auto* tracks : { &_sequence.videoTracks, &_sequence.soundTracks, &_sequence.audioTracks }) {
        for (Track& track : *tracks) {
            if (track.locked) {
                continue;
            }

            // Clips are sorted: each kept clip moves back by the length of
            // the removed clips before it.
            Time shift = 0;
            std::vector<Clip> kept;

            for (const Clip& clip : track.clips) {
                if (removed(clip)) {
                    shift += clip.duration;
                }
                else {
                    Clip moved = clip;
                    moved.start -= shift;
                    kept.push_back(moved);
                }
            }

            track.clips = std::move(kept);
        }
    }
}


bool Project::closeGap(Id trackId, Time start, Time end)
{
    Track* track = findTrack(trackId);

    if (!track || track->locked || end <= start) {
        return false;
    }

    auto emptyIn = [&](const Track& t) {
        return std::none_of(t.clips.begin(), t.clips.end(), [&](const Clip& c) { return c.start < end && c.end() > start; });
    };

    if (!emptyIn(*track)) {
        return false;
    }

    std::vector<Track*> tracks = { track };

    // The paired track (video and its sound).
    for (size_t i = 0; i < _sequence.videoTracks.size() && i < _sequence.soundTracks.size(); i++) {
        Track* pair = (&_sequence.videoTracks[i] == track) ? &_sequence.soundTracks[i]
            : (&_sequence.soundTracks[i] == track ? &_sequence.videoTracks[i] : nullptr);

        if (pair && !pair->locked && emptyIn(*pair)) {
            tracks.push_back(pair);
        }
    }

    for (Track* t : tracks) {
        for (Clip& clip : t->clips) {
            if (clip.start >= end) {
                clip.start -= end - start;
            }
        }
    }

    return true;
}


void Project::splitAt(Time t, const std::vector<Id>& onlyClips)
{
    std::vector<Id> linkGroups;

    for (Id clipId : onlyClips) {
        const Clip* clip = findClip(clipId);

        if (clip && clip->linkGroup != InvalidId) {
            linkGroups.push_back(clip->linkGroup);
        }
    }

    auto selected = [&](const Clip& clip) {
        return onlyClips.empty() ||
            std::find(onlyClips.begin(), onlyClips.end(), clip.id) != onlyClips.end() ||
            (clip.linkGroup != InvalidId && std::find(linkGroups.begin(), linkGroups.end(), clip.linkGroup) != linkGroups.end());
    };

    // The right parts of linked clips stay linked together, in a new group.
    std::vector<std::pair<Id, Id>> newLinkGroups;

    auto newLinkGroup = [&](Id oldGroup) {
        if (oldGroup == InvalidId) {
            return InvalidId;
        }

        for (const auto& [from, to] : newLinkGroups) {
            if (from == oldGroup) return to;
        }

        const Id group = newId();
        newLinkGroups.emplace_back(oldGroup, group);
        return group;
    };

    for (auto* tracks : { &_sequence.videoTracks, &_sequence.soundTracks, &_sequence.audioTracks }) {
        for (Track& track : *tracks) {
            if (track.locked) {
                continue;
            }

            const int index = track.clipAt(t);

            if (index < 0 || track.clips[index].start == t || !selected(track.clips[index])) {
                continue;
            }

            Clip& left = track.clips[index];
            Clip right = left;

            right.id = newId();
            right.start = t;
            right.duration = left.end() - t;
            right.sourceIn = left.sourceIn + (t - left.start);
            right.linkGroup = newLinkGroup(left.linkGroup);

            // The animation goes on where it was; the transition stays at the
            // start of the left part.
            right.transform.shiftKeys(-(t - left.start));
            right.transitionIn = Transition();

            // The fades stay at the outer edges.
            right.fadeIn = 0;
            left.fadeOut = 0;
            right.fadeOut = std::min(right.fadeOut, right.duration);
            left.fadeIn = std::min(left.fadeIn, t - left.start);

            left.duration = t - left.start;

            track.clips.push_back(right);
            track.sortClips();
        }
    }
}


Id Project::addTextClip(Id trackId, Time start, Time duration, const std::string& text)
{
    Track* track = findTrack(trackId);

    if (!track || track->type != TrackType::Video || track->locked || start < 0) {
        return InvalidId;
    }

    Time freeStart, freeEnd;

    if (!freeRangeAt(*track, start, InvalidId, freeStart, freeEnd)) {
        return InvalidId;
    }

    Clip clip;
    clip.id = newId();
    clip.kind = ClipKind::Text;
    clip.start = start;
    clip.duration = std::min(duration, freeEnd - start);
    clip.text.spans.push_back({ text, TextStyle() });

    if (clip.duration <= 0) {
        return InvalidId;
    }

    track->clips.push_back(clip);
    track->sortClips();

    return clip.id;
}


Id Project::insertFreezeFrame(Id clipId, Time t, Time duration)
{
    Clip* clip = findClip(clipId);
    Track* track = findTrackOfClip(clipId);

    if (!clip || !track || track->type != TrackType::Video || clip->kind != ClipKind::Media ||
        t < clip->start || t >= clip->end() || duration <= 0 || track->locked) {
        return InvalidId;
    }

    const Time frozenSource = clip->frozen ? clip->sourceIn : clip->sourceIn + (t - clip->start);
    const Id mediaId = clip->mediaId;
    const Transform transform = clip->transform;
    const Id linkGroup = clip->linkGroup;
    const Time localTime = t - clip->start;

    // Cut the clip and its linked clips at t.
    if (t > clip->start) {
        splitAt(t, { clipId });
    }

    // Everything from t on, on the tracks of the clip and of its links,
    // moves later to make room.
    std::vector<Track*> tracks = { track };

    for (auto* list : { &_sequence.videoTracks, &_sequence.soundTracks, &_sequence.audioTracks }) {
        for (Track& other : *list) {
            for (const Clip& c : other.clips) {
                if (linkGroup != InvalidId && c.linkGroup == linkGroup && &other != track) {
                    tracks.push_back(&other);
                    break;
                }
            }
        }
    }

    for (Track* shifted : tracks) {
        if (shifted->locked) {
            continue;
        }

        for (Clip& c : shifted->clips) {
            if (c.start >= t) {
                c.start += duration;
            }
        }
    }

    Clip still;
    still.id = newId();
    still.mediaId = mediaId;
    still.start = t;
    still.duration = duration;
    still.sourceIn = frozenSource;
    still.frozen = true;
    still.transform = transform;

    // The still keeps the placement of the frame at t, without animation.
    for (AnimatedValue* value : { &still.transform.x, &still.transform.y, &still.transform.scale,
                                  &still.transform.rotation, &still.transform.opacity }) {
        value->clearKeys(localTime);
    }

    track->clips.push_back(still);
    track->sortClips();

    return still.id;
}


Json Project::toJson(const std::filesystem::path& projectDir) const
{
    Json json = Json::object();
    json["format"] = "QuickEdit project";
    json["version"] = FileVersion;
    json["nextId"] = _nextId;

    Json media = Json::array();

    for (const MediaItem& item : _media) {
        Json entry = Json::object();
        entry["id"] = item.id;
        entry["path"] = pathToJson(item.path, projectDir);
        entry["type"] = toString(item.type);
        entry["probed"] = item.probed;
        entry["duration"] = item.duration;
        entry["width"] = item.width;
        entry["height"] = item.height;
        entry["frameRate"] = rationalToJson(item.frameRate);
        entry["hasAudio"] = item.hasAudio;
        entry["sampleRate"] = item.sampleRate;
        entry["channels"] = item.channels;
        media.push_back(entry);
    }

    json["media"] = media;

    Json sequence = Json::object();
    sequence["name"] = _sequence.name;
    sequence["width"] = _sequence.width;
    sequence["height"] = _sequence.height;
    sequence["frameRate"] = rationalToJson(_sequence.frameRate);
    sequence["sampleRate"] = _sequence.sampleRate;

    Json videoTracks = Json::array();
    for (const Track& track : _sequence.videoTracks) {
        videoTracks.push_back(trackToJson(track));
    }

    Json audioTracks = Json::array();
    for (const Track& track : _sequence.audioTracks) {
        audioTracks.push_back(trackToJson(track));
    }

    Json soundTracks = Json::array();
    for (const Track& track : _sequence.soundTracks) {
        soundTracks.push_back(trackToJson(track));
    }

    sequence["videoTracks"] = videoTracks;
    sequence["soundTracks"] = soundTracks;
    sequence["audioTracks"] = audioTracks;
    sequence["soundTracksFolded"] = _sequence.soundTracksFolded;

    // A single sequence for now, in an array for the future versions.
    Json sequences = Json::array();
    sequences.push_back(sequence);
    json["sequences"] = sequences;

    return json;
}


void Project::fromJson(const Json& json, const std::filesystem::path& projectDir)
{
    const int64_t version = json["version"].toInt(-1);

    if (json["format"].toString() != "QuickEdit project" || version < 1) {
        throw std::runtime_error("Not a QuickEdit project");
    }

    if (version > FileVersion) {
        throw std::runtime_error("This project was saved by a newer version of QuickEdit");
    }

    Project loaded;
    loaded._media.clear();
    loaded._sequence = Sequence();

    for (const Json& entry : json["media"].items()) {
        MediaItem item;
        item.id = (Id)entry["id"].toInt();
        item.path = pathFromJson(entry["path"].toString(), projectDir);
        item.type = mediaTypeFromString(entry["type"].toString());
        item.probed = entry["probed"].toBool();
        item.duration = entry["duration"].toInt();
        item.width = (uint32_t)entry["width"].toInt();
        item.height = (uint32_t)entry["height"].toInt();
        item.frameRate = rationalFromJson(entry["frameRate"], { 30, 1 });
        item.hasAudio = entry["hasAudio"].toBool();
        item.sampleRate = (uint32_t)entry["sampleRate"].toInt();
        item.channels = (uint32_t)entry["channels"].toInt();
        loaded._media.push_back(item);
    }

    const Json& sequence = json["sequences"][0];

    loaded._sequence.name = sequence["name"].toString("Sequence 1");
    loaded._sequence.width = (uint32_t)std::max<int64_t>(16, sequence["width"].toInt(1920));
    loaded._sequence.height = (uint32_t)std::max<int64_t>(16, sequence["height"].toInt(1080));
    loaded._sequence.frameRate = rationalFromJson(sequence["frameRate"], { 30, 1 });
    loaded._sequence.sampleRate = (uint32_t)sequence["sampleRate"].toInt(48000);

    for (const Json& track : sequence["videoTracks"].items()) {
        loaded._sequence.videoTracks.push_back(trackFromJson(track, TrackType::Video));
    }

    for (const Json& track : sequence["audioTracks"].items()) {
        loaded._sequence.audioTracks.push_back(trackFromJson(track, TrackType::Audio));
    }

    for (const Json& track : sequence["soundTracks"].items()) {
        loaded._sequence.soundTracks.push_back(trackFromJson(track, TrackType::Sound));
    }

    loaded._sequence.soundTracksFolded = sequence["soundTracksFolded"].toBool();

    // Never hand out an id already in use, even if nextId is missing.
    Id maxId = 0;

    for (const MediaItem& item : loaded._media) maxId = std::max(maxId, item.id);

    for (const auto* tracks : { &loaded._sequence.videoTracks, &loaded._sequence.soundTracks, &loaded._sequence.audioTracks }) {
        for (const Track& track : *tracks) {
            maxId = std::max(maxId, track.id);

            for (const EffectSlot& slot : track.effects) {
                maxId = std::max(maxId, slot.id);
            }

            for (const Clip& clip : track.clips) {
                maxId = std::max({ maxId, clip.id, clip.linkGroup });
            }
        }
    }

    loaded._nextId = std::max<Id>((Id)json["nextId"].toInt(), maxId + 1);

    // One sound track per video track. Projects without them: the sound of
    // each video clip moves from the audio tracks to the sound track of its
    // video track.
    const bool migrate = loaded._sequence.soundTracks.empty();

    while (loaded._sequence.soundTracks.size() < loaded._sequence.videoTracks.size()) {
        Track sound;
        sound.id = loaded.newId();
        sound.type = TrackType::Sound;
        sound.name = "S" + std::to_string(loaded._sequence.soundTracks.size() + 1);
        loaded._sequence.soundTracks.push_back(sound);
    }

    loaded._sequence.soundTracks.resize(loaded._sequence.videoTracks.size());

    if (migrate) {
        for (size_t v = 0; v < loaded._sequence.videoTracks.size(); v++) {
            for (const Clip& video : loaded._sequence.videoTracks[v].clips) {
                if (video.linkGroup == InvalidId) {
                    continue;
                }

                for (Track& audio : loaded._sequence.audioTracks) {
                    for (size_t c = 0; c < audio.clips.size(); c++) {
                        if (audio.clips[c].linkGroup == video.linkGroup) {
                            loaded._sequence.soundTracks[v].clips.push_back(audio.clips[c]);
                            audio.clips.erase(audio.clips.begin() + (ptrdiff_t)c);
                            break;
                        }
                    }
                }
            }

            loaded._sequence.soundTracks[v].sortClips();
        }
    }

    *this = std::move(loaded);
}


void Project::save(const std::filesystem::path& filepath) const
{
    const std::string text = toJson(filepath.parent_path()).dump();

    // Written next to the destination first, so that a failure never leaves
    // a truncated project behind.
    std::filesystem::path tempPath = filepath;
    tempPath += ".tmp";

    {
        std::ofstream file(tempPath, std::ios::binary | std::ios::trunc);

        if (!file) {
            throw std::runtime_error("Could not write the project file");
        }

        file.write(text.data(), (std::streamsize)text.size());

        if (!file) {
            throw std::runtime_error("Could not write the project file");
        }
    }

    std::filesystem::rename(tempPath, filepath);
}


void Project::load(const std::filesystem::path& filepath)
{
    std::ifstream file(filepath, std::ios::binary);

    if (!file) {
        throw std::runtime_error("Could not open the project file");
    }

    std::stringstream ss;
    ss << file.rdbuf();

    fromJson(Json::parse(ss.str()), filepath.parent_path());
}

}

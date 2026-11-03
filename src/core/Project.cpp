#include <core/Project.h>

#include <algorithm>
#include <cstddef>
#include <utility>
#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>


namespace core
{

const char* scaleFilterName(ScaleFilter filter)
{
    switch (filter) {
        case ScaleFilter::Nearest: return "nearest";
        case ScaleFilter::Bicubic: return "bicubic";
        case ScaleFilter::Lanczos: return "lanczos";
        default:                   return "bilinear";
    }
}


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

    if (text.motion != TextMotion::None) {
        json["motion"] = (text.motion == TextMotion::Roll) ? "roll" : "crawl";
    }

    if (text.background.enabled) {
        Json background = Json::object();
        background["color"] = colorToJson(text.background.color);
        background["padding"] = (double)text.background.padding;
        background["cornerRadius"] = (double)text.background.cornerRadius;
        background["fullWidth"] = text.background.fullWidth;
        json["background"] = background;
    }

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

    const std::string motion = json["motion"].toString();
    text.motion = (motion == "roll") ? TextMotion::Roll : (motion == "crawl") ? TextMotion::Crawl : TextMotion::None;

    if (const Json& background = json["background"]; background.isObject()) {
        text.background.enabled = true;
        text.background.color = colorFromJson(background["color"], defaults.background.color);
        text.background.padding = (float)std::max(0.0, background["padding"].toDouble(defaults.background.padding));
        text.background.cornerRadius = (float)std::max(0.0, background["cornerRadius"].toDouble(defaults.background.cornerRadius));
        text.background.fullWidth = background["fullWidth"].toBool();
    }

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


Json slotsToJson(const std::vector<EffectSlot>& slots)
{
    Json effects = Json::array();

    for (const EffectSlot& slot : slots) {
        Json entry = Json::object();
        entry["id"] = slot.id;
        entry["path"] = slot.path;
        entry["classId"] = slot.classId;
        entry["name"] = slot.name;
        entry["bypass"] = slot.bypass;
        entry["state"] = slot.state;
        effects.push_back(entry);
    }

    return effects;
}


std::vector<EffectSlot> slotsFromJson(const Json& json)
{
    std::vector<EffectSlot> slots;

    for (const Json& entry : json.items()) {
        EffectSlot slot;
        slot.id = (Id)entry["id"].toInt();
        slot.path = entry["path"].toString();
        slot.classId = entry["classId"].toString();
        slot.name = entry["name"].toString();
        slot.bypass = entry["bypass"].toBool();
        slot.state = entry["state"].toString();
        slots.push_back(slot);
    }

    return slots;
}


Json videoEffectsToJson(const std::vector<VideoEffect>& effects)
{
    Json array = Json::array();

    for (const VideoEffect& effect : effects) {
        Json entry = Json::object();
        entry["id"] = effect.id;
        entry["type"] = effect.type;
        entry["enabled"] = effect.enabled;

        Json parameters = Json::array();

        for (const AnimatedValue& value : effect.parameters) {
            parameters.push_back(animatedToJson(value));
        }

        entry["parameters"] = parameters;
        array.push_back(entry);
    }

    return array;
}


std::vector<VideoEffect> videoEffectsFromJson(const Json& json)
{
    std::vector<VideoEffect> effects;

    for (const Json& entry : json.items()) {
        // An unknown type (a newer version): dropped.
        VideoEffect effect = makeVideoEffect((Id)entry["id"].toInt(), entry["type"].toString());

        if (!effect.info()) {
            continue;
        }

        effect.enabled = entry["enabled"].toBool(true);

        const std::vector<Json> parameters = entry["parameters"].items();

        for (size_t i = 0; i < effect.parameters.size() && i < parameters.size(); i++) {
            effect.parameters[i] = animatedFromJson(parameters[i], effect.info()->parameters[i].defaultValue);
        }

        effects.push_back(std::move(effect));
    }

    return effects;
}


Json clipToJson(const Clip& clip)
{
    Json json = Json::object();
    json["id"] = clip.id;
    json["kind"] = (clip.kind == ClipKind::Text) ? "text"
        : (clip.kind == ClipKind::Generator) ? "generator"
        : (clip.kind == ClipKind::Sequence) ? "sequence" : "media";

    if (clip.kind == ClipKind::Sequence) {
        json["sequence"] = clip.sequence;
    }
    json["media"] = clip.mediaId;
    json["start"] = clip.start;
    json["duration"] = clip.duration;
    json["sourceIn"] = clip.sourceIn;
    json["link"] = clip.linkGroup;
    json["gainDb"] = (double)clip.gainDb;
    json["frozen"] = clip.frozen;

    if (clip.muted) json["muted"] = true;

    for (const auto& [name, streams] : { std::pair{ "mutedStreams", &clip.mutedStreams }, std::pair{ "removedStreams", &clip.removedStreams } }) {
        if (!streams->empty()) {
            Json list = Json::array();

            for (int stream : *streams) {
                list.push_back(stream);
            }

            json[name] = list;
        }
    }

    if (clip.speed != 1.0) json["speed"] = clip.speed;
    if (clip.audioStream >= 0) json["audioStream"] = clip.audioStream;
    if (clip.fit != FitMode::Fit) json["fit"] = clip.fit == FitMode::Fill ? "fill" : "native";
    if (clip.filter != ScaleFilter::Bilinear) json["filter"] = scaleFilterName(clip.filter);
    if (clip.blend != BlendMode::Normal) json["blend"] = blendModeName(clip.blend);

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

    if (clip.kind == ClipKind::Generator) {
        json["shader"] = clip.shader;

        Json parameters = Json::array();

        for (const AnimatedValue& value : clip.shaderParameters) {
            parameters.push_back(animatedToJson(value));
        }

        json["shaderParameters"] = parameters;
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

    if (!clip.videoEffects.empty()) {
        json["videoEffects"] = videoEffectsToJson(clip.videoEffects);
    }

    if (!clip.effects.empty()) {
        json["effects"] = slotsToJson(clip.effects);
    }

    if (clip.transitionIn.type != TransitionType::None) {
        Json transition = Json::object();
        transition["type"] = (int)clip.transitionIn.type;
        transition["duration"] = clip.transitionIn.duration;

        if (!clip.transitionIn.shader.empty()) {
            transition["shader"] = clip.transitionIn.shader;
        }
        json["transitionIn"] = transition;
    }

    return json;
}


Clip clipFromJson(const Json& json)
{
    Clip clip;
    clip.id = (Id)json["id"].toInt();
    const std::string kind = json["kind"].toString();
    clip.kind = (kind == "text") ? ClipKind::Text
        : (kind == "generator") ? ClipKind::Generator
        : (kind == "sequence") ? ClipKind::Sequence : ClipKind::Media;
    clip.sequence = (Id)json["sequence"].toInt();
    clip.mediaId = (Id)json["media"].toInt();
    clip.start = json["start"].toInt();
    clip.duration = std::max<Time>(1, json["duration"].toInt(1));
    clip.sourceIn = json["sourceIn"].toInt();
    clip.linkGroup = (Id)json["link"].toInt();
    clip.gainDb = (float)json["gainDb"].toDouble(0.0);
    clip.frozen = json["frozen"].toBool();
    clip.muted = json["muted"].toBool();

    for (const Json& stream : json["mutedStreams"].items()) clip.mutedStreams.push_back((int)stream.toInt());
    for (const Json& stream : json["removedStreams"].items()) clip.removedStreams.push_back((int)stream.toInt());
    clip.speed = std::clamp(json["speed"].toDouble(1.0), Project::MinSpeed, Project::MaxSpeed);
    clip.audioStream = (int)std::max<int64_t>(-1, json["audioStream"].toInt(-1));

    const std::string fit = json["fit"].toString();
    clip.fit = (fit == "fill") ? FitMode::Fill : (fit == "native") ? FitMode::Native : FitMode::Fit;
    clip.filter = ScaleFilter::Bilinear;

    for (int filter = 0; filter < (int)ScaleFilter::Count; filter++) {
        if (json["filter"].toString() == scaleFilterName((ScaleFilter)filter)) {
            clip.filter = (ScaleFilter)filter;
        }
    }
    clip.blend = blendModeFromName(json["blend"].toString());
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

    if (clip.kind == ClipKind::Generator) {
        clip.shader = json["shader"].toString();

        for (const Json& value : json["shaderParameters"].items()) {
            clip.shaderParameters.push_back(animatedFromJson(value, 0.f));
        }
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

    clip.videoEffects = videoEffectsFromJson(json["videoEffects"]);
    clip.effects = slotsFromJson(json["effects"]);

    const Json& transition = json["transitionIn"];
    clip.transitionIn.type = (TransitionType)std::clamp<int64_t>(transition["type"].toInt(), 0, (int64_t)TransitionType::Shader);
    clip.transitionIn.shader = transition["shader"].toString();
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

    if (track.height > 0.f) json["height"] = (double)track.height;
    if (track.blend != BlendMode::Normal) json["blend"] = blendModeName(track.blend);
    if (track.streamsUnfolded) json["streamsUnfolded"] = true;

    Json clips = Json::array();

    for (const Clip& clip : track.clips) {
        clips.push_back(clipToJson(clip));
    }

    json["clips"] = clips;

    if (!track.effects.empty()) {
        json["effects"] = slotsToJson(track.effects);
    }

    if (!track.videoEffects.empty()) {
        json["videoEffects"] = videoEffectsToJson(track.videoEffects);
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
    track.height = (float)json["height"].toDouble(0.0);
    track.blend = blendModeFromName(json["blend"].toString());
    track.streamsUnfolded = json["streamsUnfolded"].toBool();

    for (const Json& clip : json["clips"].items()) {
        track.clips.push_back(clipFromJson(clip));
    }

    track.sortClips();

    track.effects = slotsFromJson(json["effects"]);
    track.videoEffects = videoEffectsFromJson(json["videoEffects"]);

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


std::string MediaItem::audioStreamName(int stream) const
{
    if (stream >= 0 && stream < (int)audioStreams.size() && !audioStreams[(size_t)stream].empty()) {
        return audioStreams[(size_t)stream];
    }

    return "Stream " + std::to_string(stream + 1);
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

    for (const auto* tracks : { &videoTracks, &soundTracks, &audioTracks, &subtitleTracks }) {
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
    track.type = (type == TrackType::Video || type == TrackType::Subtitle) ? type : TrackType::Audio;

    if (track.type == TrackType::Subtitle) {
        track.name = "ST" + std::to_string(subtitleTracks.size() + 1);
        subtitleTracks.push_back(track);
    }
    else if (track.type == TrackType::Video) {
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


const char* fitModeName(FitMode mode)
{
    switch (mode) {
        case FitMode::Fill:   return "Fill";
        case FitMode::Native: return "Native";
        default:              return "Fit";
    }
}


float fitScale(FitMode mode, float frameWidth, float frameHeight, float pictureWidth, float pictureHeight)
{
    if (pictureWidth <= 0.f || pictureHeight <= 0.f) {
        return 1.f;
    }

    switch (mode) {
        case FitMode::Fill:   return std::max(frameWidth / pictureWidth, frameHeight / pictureHeight);
        case FitMode::Native: return 1.f;
        default:              return std::min(frameWidth / pictureWidth, frameHeight / pictureHeight);
    }
}


const char* blendModeName(BlendMode mode)
{
    switch (mode) {
        case BlendMode::Add:      return "Add";
        case BlendMode::Screen:   return "Screen";
        case BlendMode::Multiply: return "Multiply";
        case BlendMode::Lighten:  return "Lighten";
        case BlendMode::Darken:   return "Darken";
        default:                  return "Normal";
    }
}


BlendMode blendModeFromName(const std::string& name)
{
    for (int i = 0; i < (int)BlendMode::Count; i++) {
        if (name == blendModeName((BlendMode)i)) return (BlendMode)i;
    }

    return BlendMode::Normal;
}


bool Sequence::removeTrack(Id trackId)
{
    for (TrackType type : { TrackType::Video, TrackType::Sound, TrackType::Audio, TrackType::Subtitle }) {
        std::vector<Track>& list = tracks(type);
        const auto it = std::find_if(list.begin(), list.end(), [&](const Track& track) { return track.id == trackId; });

        if (it == list.end()) {
            continue;
        }

        const size_t index = (size_t)(it - list.begin());

        if (type == TrackType::Video || type == TrackType::Sound) {
            if (videoTracks.size() <= 1) {
                return false;
            }

            if (index < videoTracks.size()) videoTracks.erase(videoTracks.begin() + (std::ptrdiff_t)index);
            if (index < soundTracks.size()) soundTracks.erase(soundTracks.begin() + (std::ptrdiff_t)index);
        }
        else {
            list.erase(it);
        }

        // Default names follow the places.
        auto rename = [](std::vector<Track>& renamed, const std::string& prefix) {
            for (size_t i = 0; i < renamed.size(); i++) {
                std::string& name = renamed[i].name;
                const bool isDefault = name.size() > prefix.size() && name.compare(0, prefix.size(), prefix) == 0 &&
                    std::all_of(name.begin() + (std::ptrdiff_t)prefix.size(), name.end(), [](char c) { return c >= '0' && c <= '9'; });

                if (isDefault) {
                    name = prefix + std::to_string(i + 1);
                }
            }
        };

        rename(videoTracks, "V");
        rename(soundTracks, "S");
        rename(audioTracks, "A");
        rename(subtitleTracks, "ST");

        return true;
    }

    return false;
}


void Sequence::resize(uint32_t newWidth, uint32_t newHeight)
{
    if (newWidth == 0 || newHeight == 0 || width == 0 || height == 0) {
        return;
    }

    const float fx = (float)newWidth / (float)width;
    const float fy = (float)newHeight / (float)height;
    const float size = std::min(fx, fy);

    for (Track& track : videoTracks) {
        for (Clip& clip : track.clips) {
            clip.transform.x.scaleValues(fx);
            clip.transform.y.scaleValues(fy);

            // The pictures fitted to the frame follow it by themselves.
            if (clip.kind == ClipKind::Text || (clip.kind == ClipKind::Media && clip.fit == FitMode::Native)) {
                clip.transform.scale.scaleValues(size);
            }

            clip.frame.cornerRadius *= size;
            clip.frame.borderWidth *= size;
            clip.frame.shadowOffsetX *= size;
            clip.frame.shadowOffsetY *= size;
            clip.frame.shadowSoftness *= size;
        }
    }

    subtitleStyle.size *= size;
    subtitleStyle.outlineWidth *= size;
    subtitleStyle.bottomMargin *= fy;

    width = newWidth;
    height = newHeight;
}


Track* Sequence::findTrack(Id trackId)
{
    for (TrackType type : { TrackType::Video, TrackType::Sound, TrackType::Audio, TrackType::Subtitle }) {
        for (Track& track : tracks(type)) {
            if (track.id == trackId) return &track;
        }
    }

    return nullptr;
}


Project::Project()
{
    _sequence.id = newId();
    _sequenceOrder.push_back(_sequence.id);

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
    for (auto* tracks : { &_sequence.videoTracks, &_sequence.soundTracks, &_sequence.audioTracks, &_sequence.subtitleTracks }) {
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
    for (auto* tracks : { &_sequence.videoTracks, &_sequence.soundTracks, &_sequence.audioTracks, &_sequence.subtitleTracks }) {
        for (Track& track : *tracks) {
            if (track.id == trackId) return &track;
        }
    }

    return nullptr;
}


EffectSlot* Project::findEffect(Id slotId)
{
    for (auto* tracks : { &_sequence.soundTracks, &_sequence.audioTracks }) {
        for (Track& track : *tracks) {
            for (EffectSlot& slot : track.effects) {
                if (slot.id == slotId) return &slot;
            }

            for (Clip& clip : track.clips) {
                for (EffectSlot& slot : clip.effects) {
                    if (slot.id == slotId) return &slot;
                }
            }
        }
    }

    return nullptr;
}


std::string Project::parentFolder(const std::string& path)
{
    const size_t slash = path.rfind('/');
    return (slash == std::string::npos) ? std::string() : path.substr(0, slash);
}


std::string Project::addMediaFolder(const std::string& parent, const std::string& name)
{
    const std::string base = (parent.empty() ? "" : parent + "/") + (name.empty() ? "New Folder" : name);
    std::string path = base;

    for (int i = 2; std::find(_mediaFolders.begin(), _mediaFolders.end(), path) != _mediaFolders.end(); i++) {
        path = base + " " + std::to_string(i);
    }

    _mediaFolders.push_back(path);
    return path;
}


bool Project::renameMediaFolder(const std::string& path, const std::string& name)
{
    if (name.empty() || name.find('/') != std::string::npos) {
        return false;
    }

    const std::string parent = parentFolder(path);
    const std::string renamed = (parent.empty() ? "" : parent + "/") + name;

    if (renamed == path) {
        return true;
    }

    if (std::find(_mediaFolders.begin(), _mediaFolders.end(), renamed) != _mediaFolders.end()) {
        return false;
    }

    // The folder, and what is under it.
    auto rename = [&](std::string& folder) {
        if (folder == path) {
            folder = renamed;
        }
        else if (folder.size() > path.size() && folder.compare(0, path.size(), path) == 0 && folder[path.size()] == '/') {
            folder = renamed + folder.substr(path.size());
        }
    };

    for (std::string& folder : _mediaFolders) rename(folder);
    for (MediaItem& item : _media) rename(item.folder);

    return true;
}


void Project::removeMediaFolder(const std::string& path)
{
    const std::string parent = parentFolder(path);

    auto under = [&](const std::string& folder) {
        return folder == path || (folder.size() > path.size() && folder.compare(0, path.size(), path) == 0 && folder[path.size()] == '/');
    };

    for (MediaItem& item : _media) {
        if (under(item.folder)) {
            item.folder = parent;
        }
    }

    std::erase_if(_mediaFolders, under);
}


void Project::moveMediaToFolder(Id mediaId, const std::string& folder)
{
    if (MediaItem* item = findMedia(mediaId)) {
        item->folder = folder;

        if (!folder.empty() && std::find(_mediaFolders.begin(), _mediaFolders.end(), folder) == _mediaFolders.end()) {
            _mediaFolders.push_back(folder);
        }
    }
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


std::pair<Id, Id> Project::clipTracks(Id mediaId, Id trackId)
{
    const MediaItem* media = findMedia(mediaId);
    Track* target = findTrack(trackId);

    if (!media || !target || target->locked) {
        return { InvalidId, InvalidId };
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

    return { videoTrack ? videoTrack->id : InvalidId, audioTrack ? audioTrack->id : InvalidId };
}


Id Project::addClip(Id mediaId, Id trackId, Time start, bool overwriting)
{
    const MediaItem* media = findMedia(mediaId);

    if (!media || start < 0) {
        return InvalidId;
    }

    const auto [videoId, audioId] = clipTracks(mediaId, trackId);
    Track* videoTrack = (videoId != InvalidId) ? findTrack(videoId) : nullptr;
    Track* audioTrack = (audioId != InvalidId) ? findTrack(audioId) : nullptr;

    if (!videoTrack && !audioTrack) {
        return InvalidId;
    }

    // Room made over the whole media.
    if (overwriting) {
        for (Track* track : { videoTrack, audioTrack }) {
            if (track) {
                overwrite(track->id, start, start + media->duration);
            }
        }
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

    for (auto* tracks : { &_sequence.videoTracks, &_sequence.soundTracks, &_sequence.audioTracks, &_sequence.subtitleTracks }) {
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

    for (auto* tracks : { &_sequence.videoTracks, &_sequence.soundTracks, &_sequence.audioTracks, &_sequence.subtitleTracks }) {
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

    for (auto* tracks : { &_sequence.videoTracks, &_sequence.soundTracks, &_sequence.audioTracks, &_sequence.subtitleTracks }) {
        for (Track& track : *tracks) {
            if (track.locked) {
                continue;
            }

            const int index = track.clipAt(t);

            if (index < 0 || track.clips[index].start == t || !selected(track.clips[index])) {
                continue;
            }

            Clip right = cutClip(track.clips[index], t);
            right.linkGroup = newLinkGroup(right.linkGroup);

            track.clips.push_back(right);
            track.sortClips();
        }
    }
}


Json Project::copyClips(const std::vector<Id>& clipIds) const
{
    Json clips = Json::array();
    Time first = std::numeric_limits<Time>::max();

    for (const TrackType type : { TrackType::Video, TrackType::Sound, TrackType::Audio, TrackType::Subtitle }) {
        for (const Track& track : _sequence.tracks(type)) {
            for (const Clip& clip : track.clips) {
                if (std::find(clipIds.begin(), clipIds.end(), clip.id) != clipIds.end()) {
                    first = std::min(first, clip.start);
                }
            }
        }
    }

    for (const TrackType type : { TrackType::Video, TrackType::Sound, TrackType::Audio, TrackType::Subtitle }) {
        const std::vector<Track>& tracks = _sequence.tracks(type);

        for (size_t i = 0; i < tracks.size(); i++) {
            for (const Clip& clip : tracks[i].clips) {
                if (std::find(clipIds.begin(), clipIds.end(), clip.id) == clipIds.end()) {
                    continue;
                }

                Json entry = Json::object();
                entry["trackType"] = (int)type;
                entry["track"] = (int64_t)i;

                Clip relative = clip;
                relative.start -= first;
                entry["clip"] = clipToJson(relative);

                if (const MediaItem* media = findMedia(clip.mediaId)) {
                    const std::u8string path = media->path.u8string();
                    entry["media"] = std::string(path.begin(), path.end());
                }

                clips.push_back(entry);
            }
        }
    }

    Json copy = Json::object();
    copy["clips"] = clips;
    return copy;
}


std::vector<Id> Project::pasteClips(const Json& copy, Time at)
{
    std::vector<Id> pasted;
    std::vector<std::pair<Id, Id>> linkGroups;     // Copied, new

    for (const Json& entry : copy["clips"].items()) {
        const TrackType type = (TrackType)entry["trackType"].toInt();
        std::vector<Track>& tracks = _sequence.tracks(type);

        if (tracks.empty()) {
            continue;
        }

        Track& track = tracks[(size_t)std::clamp<int64_t>(entry["track"].toInt(), 0, (int64_t)tracks.size() - 1)];

        if (track.locked) {
            continue;
        }

        Clip clip = clipFromJson(entry["clip"]);
        clip.start += at;
        clip.id = newId();

        for (EffectSlot& slot : clip.effects) slot.id = newId();
        for (VideoEffect& effect : clip.videoEffects) effect.id = newId();

        // Its media: the same file, here.
        if (clip.kind == ClipKind::Media) {
            const std::string path = entry["media"].toString();

            if (path.empty()) {
                continue;
            }

            clip.mediaId = importMedia(std::filesystem::path(std::u8string(path.begin(), path.end())));
        }

        // A nested sequence of another project: nothing to show.
        if (clip.kind == ClipKind::Sequence && !findSequence(clip.sequence)) {
            continue;
        }

        if (clip.linkGroup != InvalidId) {
            auto it = std::find_if(linkGroups.begin(), linkGroups.end(), [&](const auto& pair) { return pair.first == clip.linkGroup; });

            if (it == linkGroups.end()) {
                linkGroups.emplace_back(clip.linkGroup, newId());
                it = linkGroups.end() - 1;
            }

            clip.linkGroup = it->second;
        }

        overwrite(track.id, clip.start, clip.end());

        track.clips.push_back(clip);
        track.sortClips();
        pasted.push_back(clip.id);
    }

    return pasted;
}


Clip Project::cutClip(Clip& left, Time t)
{
    Clip right = left;

    right.id = newId();
    right.start = t;
    right.duration = left.end() - t;
    right.sourceIn = left.sourceAt(t - left.start);

    // The animation goes on where it was; the transition stays at the
    // start of the left part.
    right.transform.shiftKeys(-(t - left.start));
    right.transitionIn = Transition();

    // Its effects: instances of their own; their keys follow.
    for (EffectSlot& slot : right.effects) {
        slot.id = newId();
    }

    for (VideoEffect& effect : right.videoEffects) {
        effect.id = newId();

        for (AnimatedValue& value : effect.parameters) {
            value.shiftKeys(-(t - left.start));
        }
    }

    // The fades stay at the outer edges.
    right.fadeIn = 0;
    left.fadeOut = 0;
    right.fadeOut = std::min(right.fadeOut, right.duration);
    left.fadeIn = std::min(left.fadeIn, t - left.start);

    left.duration = t - left.start;
    return right;
}


void Project::overwrite(Id trackId, Time from, Time to, const std::vector<Id>& keep)
{
    Track* track = findTrack(trackId);

    if (!track || to <= from) {
        return;
    }

    std::vector<Clip> clips;

    for (Clip& clip : track->clips) {
        const bool kept = std::find(keep.begin(), keep.end(), clip.id) != keep.end();

        if (kept || clip.end() <= from || clip.start >= to) {
            clips.push_back(clip);
            continue;
        }

        // Inside: gone.
        if (clip.start >= from && clip.end() <= to) {
            continue;
        }

        // Across both edges: the parts before and after.
        if (clip.start < from && clip.end() > to) {
            Clip right = cutClip(clip, to);
            Clip middle = cutClip(clip, from);
            (void)middle;
            clips.push_back(clip);
            clips.push_back(right);
            continue;
        }

        // Across the start of the range: the part before.
        if (clip.start < from) {
            cutClip(clip, from);
            clips.push_back(clip);
            continue;
        }

        // Across its end: the part after, which keeps the clip's id.
        Clip right = cutClip(clip, to);
        right.id = clip.id;

        for (size_t i = 0; i < right.effects.size(); i++) right.effects[i].id = clip.effects[i].id;
        for (size_t i = 0; i < right.videoEffects.size(); i++) right.videoEffects[i].id = clip.videoEffects[i].id;

        clips.push_back(right);
    }

    track->clips = std::move(clips);
    track->sortClips();
}


Id Project::addTextClip(Id trackId, Time start, Time duration, const std::string& text)
{
    Track* track = findTrack(trackId);

    if (!track || (track->type != TrackType::Video && track->type != TrackType::Subtitle) || track->locked || start < 0) {
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


Id Project::addGeneratorClip(Id trackId, Time start, Time duration, const ShaderDef& shader)
{
    Track* track = findTrack(trackId);

    if (!track || track->type != TrackType::Video || track->locked || start < 0 || shader.kind != ShaderKind::Generator) {
        return InvalidId;
    }

    Time freeStart, freeEnd;

    if (!freeRangeAt(*track, start, InvalidId, freeStart, freeEnd)) {
        return InvalidId;
    }

    Clip clip;
    clip.id = newId();
    clip.kind = ClipKind::Generator;
    clip.start = start;
    clip.duration = std::min(duration, freeEnd - start);
    clip.shader = shader.id;

    for (const ShaderParameter& parameter : shader.parameters) {
        clip.shaderParameters.emplace_back(parameter.defaultValue);
    }

    if (clip.duration <= 0) {
        return InvalidId;
    }

    if (shader.id.rfind("builtin:", 0) != 0) {
        storeShader(shader);
    }

    track->clips.push_back(clip);
    track->sortClips();

    return clip.id;
}


void Project::storeShader(const ShaderDef& shader)
{
    for (ShaderDef& stored : _shaders) {
        if (stored.id == shader.id) {
            stored = shader;
            return;
        }
    }

    _shaders.push_back(shader);
}


const ShaderDef* Project::findShader(const std::string& id) const
{
    for (const ShaderDef& shader : _shaders) {
        if (shader.id == id) return &shader;
    }

    for (const ShaderDef& shader : builtinShaders()) {
        if (shader.id == id) return &shader;
    }

    return nullptr;
}


bool Project::unfoldAudioStreams(Id clipId)
{
    Clip* clip = findClip(clipId);
    const Track* track = findTrackOfClip(clipId);
    const MediaItem* media = clip ? findMedia(clip->mediaId) : nullptr;

    if (!clip || !track || !media || clip->kind != ClipKind::Media || clip->audioStream >= 0 || media->audioStreams.size() < 2 ||
        (track->type != TrackType::Sound && track->type != TrackType::Audio)) {
        return false;
    }

    // The clips of the streams go together.
    if (clip->linkGroup == InvalidId) {
        clip->linkGroup = newId();
    }

    clip->audioStream = 0;

    // A copy: adding a track may move the clip in memory.
    const Clip first = *clip;
    const size_t streams = media->audioStreams.size();

    auto freeOver = [&](const Track& t) {
        return !t.locked && std::none_of(t.clips.begin(), t.clips.end(),
            [&](const Clip& c) { return c.start < first.end() && c.end() > first.start; });
    };

    for (size_t stream = 1; stream < streams; stream++) {
        Clip copy = first;
        copy.id = newId();
        copy.audioStream = (int)stream;
        copy.effects.clear();

        Track* target = nullptr;

        for (Track& t : _sequence.audioTracks) {
            if (freeOver(t)) {
                target = &t;
                break;
            }
        }

        if (!target) {
            _sequence.addTrack(TrackType::Audio, newId(), InvalidId);
            target = &_sequence.audioTracks.back();
        }

        target->clips.push_back(copy);
        target->sortClips();
    }

    return true;
}


bool Project::foldAudioStreams(Id clipId)
{
    Clip* clip = findClip(clipId);

    if (!clip || clip->audioStream < 0) {
        return false;
    }

    // The clips of the other streams of the same piece of media.
    std::vector<Id> others;

    if (clip->linkGroup != InvalidId) {
        for (auto* tracks : { &_sequence.soundTracks, &_sequence.audioTracks }) {
            for (const Track& track : *tracks) {
                for (const Clip& c : track.clips) {
                    if (c.id != clipId && c.linkGroup == clip->linkGroup && c.mediaId == clip->mediaId &&
                        c.audioStream >= 0 && c.start == clip->start) {
                        others.push_back(c.id);
                    }
                }
            }
        }
    }

    removeClips(others, false);

    if (Clip* kept = findClip(clipId)) {
        kept->audioStream = -1;
    }

    return true;
}


bool Project::setClipSpeed(Id clipId, double speed)
{
    const Clip* grabbed = findClip(clipId);

    if (!grabbed || grabbed->kind != ClipKind::Media || grabbed->frozen) {
        return false;
    }

    speed = std::clamp(speed, MinSpeed, MaxSpeed);

    const Id linkGroup = grabbed->linkGroup;
    bool changed = false;

    for (auto* list : { &_sequence.videoTracks, &_sequence.soundTracks, &_sequence.audioTracks }) {
        for (Track& track : *list) {
            if (track.locked) {
                continue;
            }

            // Back to front: the clip and the ones after it are moved.
            for (size_t i = track.clips.size(); i-- > 0;) {
                Clip& clip = track.clips[i];

                if (clip.id != clipId && (linkGroup == InvalidId || clip.linkGroup != linkGroup)) {
                    continue;
                }

                if (clip.kind != ClipKind::Media || clip.frozen || clip.speed == speed) {
                    continue;
                }

                // Same part of the media.
                const Time sourceLength = clip.toSource(clip.duration);
                clip.speed = speed;

                const Time duration = std::max<Time>(1, clip.fromSource(sourceLength));
                const Time delta = duration - clip.duration;

                clip.duration = duration;
                clip.fadeIn = std::min(clip.fadeIn, duration);
                clip.fadeOut = std::min(clip.fadeOut, duration - clip.fadeIn);

                for (size_t j = i + 1; j < track.clips.size(); j++) {
                    track.clips[j].start += delta;
                }

                changed = true;
            }
        }
    }

    return changed;
}


Id Project::insertFreezeFrame(Id clipId, Time t, Time duration)
{
    Clip* clip = findClip(clipId);
    Track* track = findTrackOfClip(clipId);

    if (!clip || !track || track->type != TrackType::Video || clip->kind != ClipKind::Media ||
        t < clip->start || t >= clip->end() || duration <= 0 || track->locked) {
        return InvalidId;
    }

    const Time frozenSource = clip->sourceAt(t - clip->start);
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

    for (auto* list : { &_sequence.videoTracks, &_sequence.soundTracks, &_sequence.audioTracks, &_sequence.subtitleTracks }) {
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


namespace {

Json sequenceToJson(const Sequence& sequence)
{
    Json json = Json::object();
    json["id"] = sequence.id;
    json["name"] = sequence.name;
    json["width"] = sequence.width;
    json["height"] = sequence.height;
    json["frameRate"] = rationalToJson(sequence.frameRate);
    json["sampleRate"] = sequence.sampleRate;

    Json videoTracks = Json::array();
    for (const Track& track : sequence.videoTracks) {
        videoTracks.push_back(trackToJson(track));
    }

    Json audioTracks = Json::array();
    for (const Track& track : sequence.audioTracks) {
        audioTracks.push_back(trackToJson(track));
    }

    Json soundTracks = Json::array();
    for (const Track& track : sequence.soundTracks) {
        soundTracks.push_back(trackToJson(track));
    }

    json["videoTracks"] = videoTracks;
    json["soundTracks"] = soundTracks;
    json["audioTracks"] = audioTracks;
    json["soundTracksFolded"] = sequence.soundTracksFolded;

    if (!sequence.subtitleTracks.empty()) {
        Json subtitleTracks = Json::array();

        for (const Track& track : sequence.subtitleTracks) {
            subtitleTracks.push_back(trackToJson(track));
        }

        json["subtitleTracks"] = subtitleTracks;
    }

    if (sequence.subtitleStyle != SubtitleStyle()) {
        Json style = Json::object();
        style["size"] = (double)sequence.subtitleStyle.size;
        style["color"] = colorToJson(sequence.subtitleStyle.color);
        style["outlineWidth"] = (double)sequence.subtitleStyle.outlineWidth;
        style["bottomMargin"] = (double)sequence.subtitleStyle.bottomMargin;
        json["subtitleStyle"] = style;
    }

    return json;
}


Sequence sequenceFromJson(const Json& json)
{
    Sequence sequence;
    sequence.id = (Id)json["id"].toInt();
    sequence.name = json["name"].toString("Sequence 1");
    sequence.width = (uint32_t)std::max<int64_t>(16, json["width"].toInt(1920));
    sequence.height = (uint32_t)std::max<int64_t>(16, json["height"].toInt(1080));
    sequence.frameRate = rationalFromJson(json["frameRate"], { 30, 1 });
    sequence.sampleRate = (uint32_t)json["sampleRate"].toInt(48000);

    for (const Json& track : json["videoTracks"].items()) {
        sequence.videoTracks.push_back(trackFromJson(track, TrackType::Video));
    }

    for (const Json& track : json["audioTracks"].items()) {
        sequence.audioTracks.push_back(trackFromJson(track, TrackType::Audio));
    }

    for (const Json& track : json["soundTracks"].items()) {
        sequence.soundTracks.push_back(trackFromJson(track, TrackType::Sound));
    }

    sequence.soundTracksFolded = json["soundTracksFolded"].toBool();

    for (const Json& track : json["subtitleTracks"].items()) {
        sequence.subtitleTracks.push_back(trackFromJson(track, TrackType::Subtitle));
    }

    if (const Json& style = json["subtitleStyle"]; style.isObject()) {
        const SubtitleStyle defaults;
        sequence.subtitleStyle.size = std::max(4.f, (float)style["size"].toDouble(defaults.size));
        sequence.subtitleStyle.color = colorFromJson(style["color"], defaults.color);
        sequence.subtitleStyle.outlineWidth = std::max(0.f, (float)style["outlineWidth"].toDouble(defaults.outlineWidth));
        sequence.subtitleStyle.bottomMargin = (float)style["bottomMargin"].toDouble(defaults.bottomMargin);
    }

    return sequence;
}

} // namespace


std::vector<const Sequence*> Project::sequences() const
{
    std::vector<const Sequence*> list;

    for (Id id : _sequenceOrder) {
        if (const Sequence* sequence = findSequence(id)) list.push_back(sequence);
    }

    // Any not in the order (never expected), after.
    if (std::find(list.begin(), list.end(), &_sequence) == list.end()) list.push_back(&_sequence);

    for (const Sequence& sequence : _otherSequences) {
        if (std::find(list.begin(), list.end(), &sequence) == list.end()) list.push_back(&sequence);
    }

    return list;
}


std::vector<Sequence*> Project::sequences()
{
    std::vector<Sequence*> list;

    for (const Sequence* sequence : std::as_const(*this).sequences()) {
        list.push_back(const_cast<Sequence*>(sequence));
    }

    return list;
}


const Sequence* Project::findSequence(Id id) const
{
    if (_sequence.id == id) {
        return &_sequence;
    }

    for (const Sequence& sequence : _otherSequences) {
        if (sequence.id == id) return &sequence;
    }

    return nullptr;
}


Id Project::addSequence(const std::string& name)
{
    Sequence sequence;
    sequence.id = newId();
    sequence.name = name;
    sequence.width = _sequence.width;
    sequence.height = _sequence.height;
    sequence.frameRate = _sequence.frameRate;
    sequence.sampleRate = _sequence.sampleRate;

    for (int i = 0; i < 2; i++) {
        const Id video = newId();
        sequence.addTrack(TrackType::Video, video, newId());
    }

    for (int i = 0; i < 2; i++) {
        sequence.addTrack(TrackType::Audio, newId(), InvalidId);
    }

    _otherSequences.push_back(std::move(sequence));
    _sequenceOrder.push_back(_otherSequences.back().id);

    return _otherSequences.back().id;
}


bool Project::switchSequence(Id id)
{
    if (id == _sequence.id) {
        return true;
    }

    for (Sequence& other : _otherSequences) {
        if (other.id == id) {
            std::swap(other, _sequence);
            return true;
        }
    }

    return false;
}


bool Project::removeSequence(Id id)
{
    if (!findSequence(id) || _otherSequences.empty()) {
        return false;
    }

    // Nested somewhere: kept.
    for (const Sequence* sequence : sequences()) {
        for (const Track& track : sequence->videoTracks) {
            for (const Clip& clip : track.clips) {
                if (clip.kind == ClipKind::Sequence && clip.sequence == id) return false;
            }
        }
    }

    if (_sequence.id == id) {
        switchSequence(_otherSequences.front().id);
    }

    std::erase_if(_otherSequences, [&](const Sequence& sequence) { return sequence.id == id; });
    std::erase(_sequenceOrder, id);

    return true;
}


bool Project::sequenceContains(Id outer, Id inner) const
{
    if (outer == inner) {
        return true;
    }

    const Sequence* sequence = findSequence(outer);

    if (!sequence) {
        return false;
    }

    for (const Track& track : sequence->videoTracks) {
        for (const Clip& clip : track.clips) {
            if (clip.kind == ClipKind::Sequence && sequenceContains(clip.sequence, inner)) return true;
        }
    }

    return false;
}


Id Project::addSequenceClip(Id sequenceId, Id trackId, Time start)
{
    const Sequence* nested = findSequence(sequenceId);
    Track* track = findTrack(trackId);

    // Never inside itself.
    if (!nested || !track || track->type != TrackType::Video || track->locked || start < 0 || sequenceContains(sequenceId, _sequence.id)) {
        return InvalidId;
    }

    Time freeStart, freeEnd;

    if (!freeRangeAt(*track, start, InvalidId, freeStart, freeEnd)) {
        return InvalidId;
    }

    Clip clip;
    clip.id = newId();
    clip.kind = ClipKind::Sequence;
    clip.sequence = sequenceId;
    clip.start = start;
    clip.duration = std::min(nested->duration() > 0 ? nested->duration() : 5 * TicksPerSecond, freeEnd - start);

    if (clip.duration <= 0) {
        return InvalidId;
    }

    track->clips.push_back(clip);
    track->sortClips();

    return clip.id;
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

        if (!item.audioStreams.empty()) {
            Json streams = Json::array();
            for (const std::string& stream : item.audioStreams) streams.push_back(stream);
            entry["audioStreams"] = streams;
        }

        if (!item.folder.empty()) {
            entry["folder"] = item.folder;
        }

        media.push_back(entry);
    }

    json["media"] = media;

    if (!_shaders.empty()) {
        Json shaders = Json::array();

        for (const ShaderDef& shader : _shaders) {
            Json entry = Json::object();
            entry["id"] = shader.id;
            entry["name"] = shader.name;
            entry["source"] = shader.source;
            shaders.push_back(entry);
        }

        json["shaders"] = shaders;
    }

    if (!_mediaFolders.empty()) {
        Json folders = Json::array();

        for (const std::string& folder : _mediaFolders) {
            folders.push_back(folder);
        }

        json["mediaFolders"] = folders;
    }

    // The sequences in the order of their tabs, the current one marked.
    Json sequences = Json::array();

    for (const Sequence* sequence : this->sequences()) {
        sequences.push_back(sequenceToJson(*sequence));
    }

    json["sequences"] = sequences;
    json["currentSequence"] = _sequence.id;

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

        for (const Json& stream : entry["audioStreams"].items()) {
            item.audioStreams.push_back(stream.toString());
        }
        item.folder = entry["folder"].toString();
        loaded._media.push_back(item);
    }

    for (const Json& entry : json["shaders"].items()) {
        ShaderDef shader;

        if (parseShader(entry["source"].toString(), shader)) {
            shader.id = entry["id"].toString();
            shader.name = entry["name"].toString();
            loaded._shaders.push_back(std::move(shader));
        }
    }

    for (const Json& folder : json["mediaFolders"].items()) {
        if (!folder.toString().empty()) {
            loaded._mediaFolders.push_back(folder.toString());
        }
    }

    // The folders of the media, even when not listed.
    for (const MediaItem& item : loaded._media) {
        for (std::string folder = item.folder; !folder.empty(); folder = parentFolder(folder)) {
            if (std::find(loaded._mediaFolders.begin(), loaded._mediaFolders.end(), folder) == loaded._mediaFolders.end()) {
                loaded._mediaFolders.push_back(folder);
            }
        }
    }

    std::vector<Sequence> loadedSequences;

    for (const Json& sequence : json["sequences"].items()) {
        loadedSequences.push_back(sequenceFromJson(sequence));
    }

    if (loadedSequences.empty()) {
        loadedSequences.emplace_back();
    }

    // Never hand out an id already in use, even if nextId is missing.
    Id maxId = 0;

    for (const MediaItem& item : loaded._media) maxId = std::max(maxId, item.id);

    for (const Sequence& sequence : loadedSequences) {
    maxId = std::max(maxId, sequence.id);

    for (const auto* tracks : { &sequence.videoTracks, &sequence.soundTracks, &sequence.audioTracks, &sequence.subtitleTracks }) {
        for (const Track& track : *tracks) {
            maxId = std::max(maxId, track.id);

            for (const EffectSlot& slot : track.effects) {
                maxId = std::max(maxId, slot.id);
            }

            for (const VideoEffect& effect : track.videoEffects) {
                maxId = std::max(maxId, effect.id);
            }

            for (const Clip& clip : track.clips) {
                maxId = std::max({ maxId, clip.id, clip.linkGroup });

                for (const EffectSlot& slot : clip.effects) maxId = std::max(maxId, slot.id);
                for (const VideoEffect& effect : clip.videoEffects) maxId = std::max(maxId, effect.id);
            }
        }
    }
    }

    loaded._nextId = std::max<Id>((Id)json["nextId"].toInt(), maxId + 1);

    for (Sequence& sequence : loadedSequences) {
        // Older files: sequences without ids.
        if (sequence.id == InvalidId) {
            sequence.id = loaded.newId();
        }

        // One sound track per video track. Projects without them: the sound of
        // each video clip moves from the audio tracks to the sound track of its
        // video track.
        const bool migrate = sequence.soundTracks.empty();

        while (sequence.soundTracks.size() < sequence.videoTracks.size()) {
            Track sound;
            sound.id = loaded.newId();
            sound.type = TrackType::Sound;
            sound.name = "S" + std::to_string(sequence.soundTracks.size() + 1);
            sequence.soundTracks.push_back(sound);
        }

        sequence.soundTracks.resize(sequence.videoTracks.size());

        if (migrate) {
            for (size_t v = 0; v < sequence.videoTracks.size(); v++) {
                for (const Clip& video : sequence.videoTracks[v].clips) {
                    if (video.linkGroup == InvalidId) {
                        continue;
                    }

                    for (Track& audio : sequence.audioTracks) {
                        for (size_t c = 0; c < audio.clips.size(); c++) {
                            if (audio.clips[c].linkGroup == video.linkGroup) {
                                sequence.soundTracks[v].clips.push_back(audio.clips[c]);
                                audio.clips.erase(audio.clips.begin() + (std::ptrdiff_t)c);
                                break;
                            }
                        }
                    }
                }

                sequence.soundTracks[v].sortClips();
            }
        }
    }

    // The current one edited, the others apart, in their order.
    const Id current = (Id)json["currentSequence"].toInt(loadedSequences.front().id);
    loaded._sequenceOrder.clear();
    loaded._otherSequences.clear();

    for (Sequence& sequence : loadedSequences) {
        loaded._sequenceOrder.push_back(sequence.id);

        if (sequence.id == current) {
            loaded._sequence = std::move(sequence);
        }
        else {
            loaded._otherSequences.push_back(std::move(sequence));
        }
    }

    if (loaded._sequence.id != current) {
        loaded._sequence = std::move(loaded._otherSequences.front());
        loaded._otherSequences.erase(loaded._otherSequences.begin());
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

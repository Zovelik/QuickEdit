// The tools of the MCP server (app/McpServer.h): what an AI assistant can
// see and do in QuickEdit. They run on the main thread, between two frames,
// on the project shown: each edit is a step of the history ("AI: ...").

#include <app/GUI.h>
#include <app/I18n.h>

#include <core/Layers.h>
#include <core/Subtitles.h>
#include <core/VideoEffects.h>

#include <vkutils/Buffer.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <fstream>


using namespace core;


namespace {

constexpr int DefaultMcpPort = 8765;

double seconds(Time t)
{
    return std::round(timeToSeconds(t) * 1000.0) / 1000.0;
}


Time ticks(const Json& value, double defaultSeconds = 0.0)
{
    return secondsToTime(value.toDouble(defaultSeconds));
}


const char* trackTypeName(TrackType type)
{
    switch (type) {
        case TrackType::Video:    return "video";
        case TrackType::Sound:    return "sound";
        case TrackType::Audio:    return "audio";
        case TrackType::Subtitle: return "subtitle";
    }

    return "";
}


const char* mediaTypeName(MediaType type)
{
    switch (type) {
        case MediaType::Video: return "video";
        case MediaType::Audio: return "audio";
        case MediaType::Image: return "image";
    }

    return "";
}


struct TransitionName
{
    const char* name;
    TransitionType type;
};

const TransitionName TransitionNames[] = {
    { "none", TransitionType::None },
    { "dissolve", TransitionType::Dissolve },
    { "dip_to_black", TransitionType::DipToBlack },
    { "push", TransitionType::Push },
    { "wipe", TransitionType::Wipe },
    { "zoom", TransitionType::Zoom },
    { "shader", TransitionType::Shader },
};


// Frame rates as fractions (29.97: 30000 / 1001).
Rational rateOf(double fps)
{
    for (const Rational rate : { Rational{ 24000, 1001 }, Rational{ 30000, 1001 }, Rational{ 60000, 1001 } }) {
        if (std::abs(rate.toDouble() - fps) < 0.01) return rate;
    }

    return { std::max<int64_t>(1, std::llround(fps)), 1 };
}


// "#RRGGBB" or "#RRGGBBAA".
bool parseColor(const std::string& text, Color& color)
{
    if (text.size() != 7 && text.size() != 9) {
        return false;
    }

    if (text[0] != '#') {
        return false;
    }

    try {
        const unsigned long value = std::stoul(text.substr(1), nullptr, 16);
        const bool alpha = text.size() == 9;
        const unsigned long rgb = alpha ? value >> 8 : value;
        color.r = (float)((rgb >> 16) & 255) / 255.f;
        color.g = (float)((rgb >> 8) & 255) / 255.f;
        color.b = (float)(rgb & 255) / 255.f;
        color.a = alpha ? (float)(value & 255) / 255.f : 1.f;
        return true;
    }
    catch (const std::exception&) {
        return false;
    }
}


// RGBA pixels as a PNG, reduced to width (its shape kept).
std::vector<uint8_t> encodePng(const uint8_t* rgba, int width, int height, int outWidth)
{
    outWidth = std::clamp(outWidth, 16, width);
    const int outHeight = std::max(2, (int)std::lround((double)height * outWidth / width));
    std::vector<uint8_t> png;

    const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_PNG);
    AVCodecContext* context = codec ? avcodec_alloc_context3(codec) : nullptr;

    if (!context) {
        return png;
    }

    context->width = outWidth;
    context->height = outHeight;
    context->pix_fmt = AV_PIX_FMT_RGB24;
    context->time_base = { 1, 25 };

    AVFrame* frame = av_frame_alloc();
    AVPacket* packet = av_packet_alloc();
    SwsContext* sws = sws_getContext(width, height, AV_PIX_FMT_RGBA, outWidth, outHeight, AV_PIX_FMT_RGB24, SWS_AREA, nullptr, nullptr, nullptr);

    frame->format = AV_PIX_FMT_RGB24;
    frame->width = outWidth;
    frame->height = outHeight;

    if (sws && avcodec_open2(context, codec, nullptr) >= 0 && av_frame_get_buffer(frame, 0) >= 0) {
        const uint8_t* source[4] = { rgba, nullptr, nullptr, nullptr };
        const int sourceLines[4] = { width * 4, 0, 0, 0 };
        sws_scale(sws, source, sourceLines, 0, height, frame->data, frame->linesize);

        if (avcodec_send_frame(context, frame) >= 0 && avcodec_send_frame(context, nullptr) >= 0) {
            while (avcodec_receive_packet(context, packet) >= 0) {
                png.insert(png.end(), packet->data, packet->data + packet->size);
                av_packet_unref(packet);
            }
        }
    }

    sws_freeContext(sws);
    av_packet_free(&packet);
    av_frame_free(&frame);
    avcodec_free_context(&context);

    return png;
}


// A tool: its name, what it does, its arguments (JSON schema properties)
// and those required.
Json tool(const char* name, const char* description, const char* properties = "{}", const char* required = "[]")
{
    Json schema = Json::object();
    schema["type"] = "object";
    schema["properties"] = Json::parse(properties);
    schema["required"] = Json::parse(required);

    Json definition = Json::object();
    definition["name"] = name;
    definition["description"] = description;
    definition["inputSchema"] = schema;
    return definition;
}

} // namespace


// ----------------------------------------------------------------------------
// Definitions
// ----------------------------------------------------------------------------

Json GUI::mcpTools()
{
    // The video effects and their parameters, for add_effect.
    std::string effects;

    for (const VideoEffectInfo& info : videoEffectInfos()) {
        effects += std::string(effects.empty() ? "" : "; ") + info.id + " (";

        for (size_t i = 0; i < info.parameters.size(); i++) {
            const VideoEffectParameter& p = info.parameters[i];
            char range[96];
            std::snprintf(range, sizeof(range), "%s%s %g..%g", i ? ", " : "", p.name, p.minimum, p.maximum);
            effects += range;
        }

        effects += ")";
    }

    static const std::string addEffect = "Adds a video effect to a clip or a track (clip_id or track_id). Types and their parameters: " + effects +
        ". parameters: values by name. Keyframes: change the effect's \"parameters\" in update_clip.";

    Json tools = Json::array();

    // Seeing
    tools.push_back(tool("get_project",
        "The project shown in QuickEdit: the current sequence (size, frame rate, length), its tracks with their clips (times in seconds), "
        "the other sequences, the media (length, size, audio), the playhead. Start with this."));
    tools.push_back(tool("get_clip",
        "A clip as stored in the project (JSON, times in ticks: 705600000 per second): text spans and styles, transform with keyframes, "
        "effects, frame, transition... Change it with update_clip.",
        R"({"clip_id": {"type": "integer"}})", R"(["clip_id"])"));
    tools.push_back(tool("render_frame",
        "The picture of the current sequence at a time, as QuickEdit composites it (PNG). Use it to check a result.",
        R"({"time": {"type": "number", "description": "Seconds"}, "width": {"type": "integer", "description": "Width of the picture, 960 by default"}})",
        R"(["time"])"));
    tools.push_back(tool("transcribe_media",
        "The speech of a media (video or audio) with its times, by speech recognition (whisper.cpp, a model must be downloaded in QuickEdit). "
        "Can take a minute.",
        R"({"media_id": {"type": "integer"}, "language": {"type": "string", "description": "auto (default), en, fr..."}})", R"(["media_id"])"));
    tools.push_back(tool("list_shaders",
        "The shaders (built in and the user's): generators (pictures), transitions, blend modes, resampling; their ids and parameters."));

    // Media and structure
    tools.push_back(tool("import_media",
        "Imports files (videos, sounds, images) into the project and waits for their analysis. Returns their ids, lengths and sizes.",
        R"({"paths": {"type": "array", "items": {"type": "string"}}})", R"(["paths"])"));
    tools.push_back(tool("create_sequence",
        "A new sequence (an empty timeline), shown at once.",
        R"({"name": {"type": "string"}, "width": {"type": "integer"}, "height": {"type": "integer"}, "fps": {"type": "number"}})"));
    tools.push_back(tool("switch_sequence", "Shows another sequence of the project (the others tools work on the one shown).",
        R"({"sequence_id": {"type": "integer"}})", R"(["sequence_id"])"));
    tools.push_back(tool("update_sequence",
        "Changes the current sequence: name, size (the layout keeps its place), frame rate.",
        R"({"name": {"type": "string"}, "width": {"type": "integer"}, "height": {"type": "integer"}, "fps": {"type": "number"}})"));
    tools.push_back(tool("add_track", "Adds a track: video (with its sound track), audio or subtitle. A new video track goes on top of the others.",
        R"({"type": {"type": "string", "enum": ["video", "audio", "subtitle"]}})", R"(["type"])"));

    // Clips
    tools.push_back(tool("add_clip",
        "Places a media on a track (a video brings its sound, linked, on the sound track of the video track). "
        "source_in / duration: the part of the media used (seconds). overwrite: replace what is there instead of failing.",
        R"({"media_id": {"type": "integer"}, "track_id": {"type": "integer"}, "start": {"type": "number"}, "source_in": {"type": "number"},
            "duration": {"type": "number"}, "overwrite": {"type": "boolean"}})",
        R"(["media_id", "track_id", "start"])"));
    tools.push_back(tool("add_text",
        "A text clip on a video track (or a subtitle track). size: pixels of the sequence; color: #RRGGBB; x, y: offset of its center from the "
        "center of the frame (pixels, y down); align: left, center, right. More (several styles, animations, background, motion): update_clip.",
        R"({"track_id": {"type": "integer"}, "start": {"type": "number"}, "duration": {"type": "number"}, "text": {"type": "string"},
            "size": {"type": "number"}, "color": {"type": "string"}, "bold": {"type": "boolean"}, "x": {"type": "number"}, "y": {"type": "number"},
            "align": {"type": "string", "enum": ["left", "center", "right"]}})",
        R"(["track_id", "start", "duration", "text"])"));
    tools.push_back(tool("add_title",
        "A ready-made title at a time: end_credits (rolling), lower_third (a name and a role), news_bar (a title and a crawl). Change its texts with update_clip.",
        R"({"preset": {"type": "string", "enum": ["end_credits", "lower_third", "news_bar"]}, "start": {"type": "number"}})", R"(["preset", "start"])"));
    tools.push_back(tool("add_subtitle", "A line of subtitles (on the first subtitle track, made if needed).",
        R"({"start": {"type": "number"}, "duration": {"type": "number"}, "text": {"type": "string"}})", R"(["start", "duration", "text"])"));
    tools.push_back(tool("add_generator",
        "A clip made by a generator shader (see list_shaders) on a video track. parameters: values by name.",
        R"({"shader_id": {"type": "string"}, "track_id": {"type": "integer"}, "start": {"type": "number"}, "duration": {"type": "number"},
            "parameters": {"type": "object"}})",
        R"(["shader_id", "track_id", "start", "duration"])"));
    tools.push_back(tool("update_clip",
        "Changes a clip: the given fields replace those of its JSON (see get_clip; times in ticks, 705600000 per second). "
        "Objects are merged (e.g. {\"transform\": {\"opacity\": 0.5}} keeps x, y...), other values replaced; arrays (keyframes, spans, effects) "
        "are given whole. E.g. {\"blendShader\": \"builtin:overlay\"}, {\"text\": {\"align\": 0}}. Fields it does not know are reported. "
        "overwrite: replace the clips it would cover on its track.",
        R"({"clip_id": {"type": "integer"}, "changes": {"type": "object"}, "overwrite": {"type": "boolean"}})", R"(["clip_id", "changes"])"));
    tools.push_back(tool("move_clip",
        "Moves a clip (and its linked clips, by the same time) to a start, and optionally to another track of its kind.",
        R"({"clip_id": {"type": "integer"}, "start": {"type": "number"}, "track_id": {"type": "integer"}, "with_links": {"type": "boolean"},
            "overwrite": {"type": "boolean"}})",
        R"(["clip_id", "start"])"));
    tools.push_back(tool("split_clips", "Cuts the clips at a time (only the given ones and their links, else all of the sequence).",
        R"({"time": {"type": "number"}, "clip_ids": {"type": "array", "items": {"type": "integer"}}})", R"(["time"])"));
    tools.push_back(tool("delete_clips",
        "Removes clips (with their linked clips unless with_links is false). ripple: the clips after them move back.",
        R"({"clip_ids": {"type": "array", "items": {"type": "integer"}}, "ripple": {"type": "boolean"}, "with_links": {"type": "boolean"}})",
        R"(["clip_ids"])"));
    tools.push_back(tool("set_transition",
        "The transition into a clip from the clip before it on its track, centered on the cut: none, dissolve, dip_to_black, push, wipe, zoom, "
        "or shader (shader_id of a transition shader).",
        R"({"clip_id": {"type": "integer"}, "type": {"type": "string"}, "duration": {"type": "number"}, "shader_id": {"type": "string"}})",
        R"(["clip_id", "type"])"));
    tools.push_back(tool("add_effect", addEffect.c_str(),
        R"({"type": {"type": "string"}, "clip_id": {"type": "integer"}, "track_id": {"type": "integer"}, "parameters": {"type": "object"}})",
        R"(["type"])"));

    // Shaders
    tools.push_back(tool("write_shader",
        "Creates or replaces a shader of the user (GLSL, Documents/QuickEdit/Shaders/<name>.glsl) and compiles it: its id, or the errors (with "
        "their lines) to fix. Its kind comes from the function it defines: vec4 generate(vec2 uv, float time) (iResolution, iTime, iDuration, "
        "param(i)); vec4 transition(vec2 uv) (getFromColor(uv), getToColor(uv), progress, ratio); vec3 blend(vec3 base, vec3 layer); "
        "vec4 resample(vec2 uv) (source(ivec2), iSourceSize, iResolution). All get the sound: iLevel, iBass, iMid, iTreble (0 to 1). "
        "Parameters (generators, up to 8): comment lines '// @param Name min max default'.",
        R"({"name": {"type": "string"}, "source": {"type": "string"}})", R"(["name", "source"])"));

    // Project
    tools.push_back(tool("set_playhead", "Moves the playhead of QuickEdit (what the user sees).",
        R"({"time": {"type": "number"}})", R"(["time"])"));
    tools.push_back(tool("undo", "Undoes the last edit (the user's or yours)."));
    tools.push_back(tool("redo", "Redoes the last edit undone."));
    tools.push_back(tool("save_project", "Saves the project (to path when given, a .qep file).",
        R"({"path": {"type": "string"}})"));
    tools.push_back(tool("export_video",
        "Exports the current sequence to a video file and waits for the end. preset: YouTube, Shorts / Reels / TikTok, Vimeo, X (Twitter), Facebook, "
        "Archive (best quality), Quick preview. codec: h264, h265, av1. height: of the picture (0: the sequence's).",
        R"({"path": {"type": "string"}, "preset": {"type": "string"}, "codec": {"type": "string"}, "height": {"type": "integer"},
            "bitrate_mbps": {"type": "number"}})",
        R"(["path"])"));

    return tools;
}


std::string GUI::mcpInstructions() const
{
    return "QuickEdit is a video editor; these tools act on the project the user has open, live (each edit can be undone, by you or by the user). "
        "Call get_project first. Times are in seconds, except in the JSON of get_clip / update_clip (ticks, 705600000 per second). "
        "Video tracks are listed from the bottom one (V1) to the top one: a track covers those listed before it; each has a sound track for the sound of its videos; audio tracks hold music and voice. "
        "Positions are in pixels of the sequence, from the center of the frame. Check your work with render_frame. "
        "Shaders (write_shader) can make pictures, transitions, blend modes and resampling, and follow the sound.";
}


// ----------------------------------------------------------------------------
// Server
// ----------------------------------------------------------------------------

void GUI::setMcpEnabled(bool enabled)
{
    _mcpEnabled = enabled;
    _mcpError.clear();

    if (!enabled) {
        _mcp.stop();
        return;
    }

    if (!_mcp.start(_mcpPort, mcpTools(), mcpInstructions(), _mcpError)) {
        SDL_Log("MCP server: %s", _mcpError.c_str());
    }
}


void GUI::updateMcp()
{
    if (!_mcp.running()) {
        return;
    }

    _mcp.process([this](const std::string& name, const Json& arguments, int attempt) {
        return mcpCall(name, arguments, attempt);
    });
}


int GUI::defaultMcpPort()
{
    return DefaultMcpPort;
}


// ----------------------------------------------------------------------------
// Frame capture (render_frame)
// ----------------------------------------------------------------------------

void GUI::cmdCaptureFrame(VkCommandBuffer commandBuffer)
{
    FrameCapture& capture = _frameCapture;

    if (!capture.requested || capture.recorded) {
        return;
    }

    // Once the pictures of that time are decoded (or after a while).
    capture.waited++;

    if (_playhead != capture.time || (capture.waited < 3) || (_viewerLoading && capture.waited < 300)) {
        return;
    }

    GPU::Image* output = _compositor->output();

    if (!output) {
        return;
    }

    capture.width = output->width();
    capture.height = output->height();
    capture.buffer = std::make_unique<GPU::Buffer>("MCP frame capture", device(), (VkDeviceSize)capture.width * capture.height * 4,
        VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    capture.buffer->bind();

    output->cmdBarrierTransition(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

    VkBufferImageCopy region = {};
    region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    region.imageExtent = { capture.width, capture.height, 1 };
    vkCmdCopyImageToBuffer(commandBuffer, output->image(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, capture.buffer->buffer(), 1, &region);

    output->cmdBarrierTransition(commandBuffer, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    VkMemoryBarrier hostBarrier = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
    hostBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    hostBarrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &hostBarrier, 0, nullptr, 0, nullptr);

    capture.recorded = true;
}


// ----------------------------------------------------------------------------
// Calls
// ----------------------------------------------------------------------------

std::optional<Json> GUI::mcpCall(const std::string& name, const Json& args, int attempt)
{
    using Result = std::optional<Json>;
    Sequence& sequence = _project.sequence();

    auto error = [](const std::string& text) -> Result { return McpServer::textResult(text, true); };
    auto done = [](const Json& value) -> Result { return McpServer::jsonResult(value); };

    // An edit made: a step of the history, shown at once.
    auto edited = [this](const std::string& label) {
        commit("AI: " + label);
    };

    auto trackFromArgs = [&](const char* key) -> Track* {
        return args.contains(key) ? _project.findTrack((Id)args[key].toInt()) : nullptr;
    };

    auto mediaSummary = [](const MediaItem& item) {
        Json media = Json::object();
        media["id"] = item.id;
        media["name"] = item.name();
        const std::u8string path = item.path.u8string();
        media["path"] = std::string(path.begin(), path.end());
        media["type"] = mediaTypeName(item.type);
        media["analyzed"] = item.probed;
        media["duration"] = seconds(item.duration);

        if (item.type != MediaType::Audio) {
            media["width"] = item.width;
            media["height"] = item.height;
        }

        if (item.type == MediaType::Video) media["fps"] = item.frameRate.toDouble();
        media["hasAudio"] = item.hasAudio;
        if (item.audioStreams.size() > 1) media["audioStreams"] = (int64_t)item.audioStreams.size();
        return media;
    };

    // ------------------------------------------------------------------------
    if (name == "get_project") {
        Json project = Json::object();
        const std::u8string path = _projectPath.u8string();
        project["file"] = std::string(path.begin(), path.end());
        project["unsavedChanges"] = _history.modified();
        project["playhead"] = seconds(_playhead);

        Json current = Json::object();
        current["id"] = sequence.id;
        current["name"] = sequence.name;
        current["width"] = sequence.width;
        current["height"] = sequence.height;
        current["fps"] = sequence.frameRate.toDouble();
        current["duration"] = seconds(sequence.duration());

        Json tracks = Json::array();

        for (const TrackType type : { TrackType::Video, TrackType::Sound, TrackType::Audio, TrackType::Subtitle }) {
            for (const Track& track : sequence.tracks(type)) {
                Json t = Json::object();
                t["id"] = track.id;
                t["type"] = trackTypeName(type);
                t["name"] = track.name;
                if (track.muted) t["muted"] = true;
                if (track.locked) t["locked"] = true;
                if (!track.videoEffects.empty()) t["effects"] = (int64_t)track.videoEffects.size();
                if (!track.blendShader.empty()) t["blendShader"] = track.blendShader;
                else if (track.blend != BlendMode::Normal) t["blend"] = blendModeName(track.blend);

                Json clips = Json::array();

                for (const Clip& clip : track.clips) {
                    Json c = Json::object();
                    c["id"] = clip.id;
                    c["start"] = seconds(clip.start);
                    c["end"] = seconds(clip.end());

                    if (clip.kind == ClipKind::Text) {
                        c["text"] = clip.text.plainText();
                    }
                    else if (clip.kind == ClipKind::Generator) {
                        c["generator"] = clip.shader;
                    }
                    else if (clip.kind == ClipKind::Sequence) {
                        c["sequence"] = clip.sequence;
                    }
                    else if (const MediaItem* media = _project.findMedia(clip.mediaId)) {
                        c["media"] = media->id;
                        c["name"] = media->name();
                        c["sourceIn"] = seconds(clip.sourceIn);
                    }

                    if (clip.linkGroup != InvalidId) c["link"] = clip.linkGroup;
                    if (clip.speed != 1.0) c["speed"] = clip.speed;
                    if (clip.transitionIn.type != TransitionType::None) {
                        c["transitionIn"] = clip.transitionIn.type == TransitionType::Shader ? clip.transitionIn.shader : transitionName(clip.transitionIn.type);
                    }

                    if (!clip.videoEffects.empty()) {
                        Json effects = Json::array();
                        for (const VideoEffect& effect : clip.videoEffects) effects.push_back(effect.type);
                        c["effects"] = effects;
                    }

                    clips.push_back(c);
                }

                t["clips"] = clips;
                tracks.push_back(t);
            }
        }

        current["tracks"] = tracks;
        project["sequence"] = current;

        Json sequences = Json::array();

        for (const Sequence* other : _project.sequences()) {
            Json s = Json::object();
            s["id"] = other->id;
            s["name"] = other->name;
            sequences.push_back(s);
        }

        project["sequences"] = sequences;

        Json media = Json::array();

        for (const MediaItem& item : _project.media()) {
            media.push_back(mediaSummary(item));
        }

        project["media"] = media;
        return done(project);
    }

    // ------------------------------------------------------------------------
    if (name == "get_clip") {
        const Id id = (Id)args["clip_id"].toInt();
        const Json clip = _project.clipJson(id);

        if (clip.isNull()) {
            return error("No clip " + std::to_string(id) + " in the current sequence.");
        }

        Json answer = Json::object();
        answer["ticksPerSecond"] = (int64_t)TicksPerSecond;
        answer["track"] = _project.findTrackOfClip(id)->id;
        answer["clip"] = clip;
        return done(answer);
    }

    // ------------------------------------------------------------------------
    if (name == "render_frame") {
        if (attempt == 0) {
            stopPlayback();
            seek(std::max<Time>(0, snapToFrame(ticks(args["time"]), sequence.frameRate)));
            _frameCapture = FrameCapture();
            _frameCapture.requested = true;
            _frameCapture.time = _playhead;
            return std::nullopt;
        }

        // Recorded at a frame: read once the GPU is done with it.
        if (!_frameCapture.recorded) {
            return std::nullopt;
        }

        if (!_frameCapture.readable) {
            _frameCapture.readable = true;
            return std::nullopt;
        }

        vkDeviceWaitIdle(device().device());

        const int width = (int)std::clamp<int64_t>(args["width"].toInt(960), 16, 3840);
        const std::vector<uint8_t> png = encodePng((const uint8_t*)_frameCapture.buffer->mappedMemory(),
            (int)_frameCapture.width, (int)_frameCapture.height, width);
        const Time time = _frameCapture.time;
        _frameCapture = FrameCapture();

        if (png.empty()) {
            return error("Could not make the picture.");
        }

        char caption[96];
        std::snprintf(caption, sizeof(caption), "Sequence \"%s\" at %.3f s", sequence.name.c_str(), timeToSeconds(time));
        return McpServer::imageResult(png, caption);
    }

    // ------------------------------------------------------------------------
    if (name == "transcribe_media") {
        const Id mediaId = (Id)args["media_id"].toInt();

        if (attempt == 0) {
            if (_mcpTranscriber.running()) {
                return error("A transcription is already running.");
            }

            const MediaItem* item = _project.findMedia(mediaId);

            if (!item || !item->hasAudio) {
                return error("No media " + std::to_string(mediaId) + " with sound.");
            }

            if (!speechRecognitionAvailable()) {
                return error("This build of QuickEdit has no speech recognition.");
            }

            // The model chosen in the subtitle window, else the best one there.
            const WhisperModel* chosen = nullptr;

            for (const WhisperModel& model : whisperModels()) {
                if (modelDownloaded(model) && (!chosen || model.id == _whisperModel)) chosen = &model;
            }

            if (!chosen) {
                return error("No speech recognition model downloaded: Edit > Generate Subtitles... in QuickEdit to get one.");
            }

            _mcpTranscribeModel = modelFolder() / chosen->file;
            _mcpTranscribeLanguage = args["language"].toString("auto");
            _mcpTranscribeStarted = false;
        }

        // The sound once decoded: 16 kHz mono for the model.
        if (!_mcpTranscribeStarted) {
            const std::shared_ptr<const media::AudioBuffer> audio = _media.audio(mediaId, -1);

            if (!audio) {
                return (attempt < 6000) ? Result() : error("The sound of the media could not be decoded.");
            }

            const double step = (double)audio->sampleRate / 16000.0;
            const int64_t frames = audio->frameCount();
            std::vector<float> samples((size_t)std::max<double>(0.0, (double)frames / step));

            for (size_t i = 0; i < samples.size(); i++) {
                const int64_t index = std::min<int64_t>((int64_t)((double)i * step), frames - 1);
                samples[i] = 0.5f * (audio->samples[(size_t)index * 2] + audio->samples[(size_t)index * 2 + 1]);
            }

            SubtitleGenerator::Settings settings;
            settings.model = _mcpTranscribeModel;
            settings.language = _mcpTranscribeLanguage;
            _mcpTranscriber.start(std::move(samples), settings);
            _mcpTranscribeStarted = true;
            return std::nullopt;
        }

        if (!_mcpTranscriber.done()) {
            return std::nullopt;
        }

        _mcpTranscriber.acknowledge();

        if (const std::string failure = _mcpTranscriber.error(); !failure.empty()) {
            return error(failure);
        }

        Json cues = Json::array();

        for (const SubtitleCue& cue : _mcpTranscriber.cues()) {
            Json c = Json::object();
            c["start"] = seconds(cue.start);
            c["end"] = seconds(cue.end);
            c["text"] = cue.text;
            cues.push_back(c);
        }

        Json answer = Json::object();
        answer["media_id"] = mediaId;
        answer["segments"] = cues;
        return done(answer);
    }

    // ------------------------------------------------------------------------
    if (name == "list_shaders") {
        Json list = Json::array();

        auto add = [&](const ShaderDef& shader) {
            Json s = Json::object();
            s["id"] = shader.id;
            s["name"] = shader.name;
            s["kind"] = shaderKindName(shader.kind);

            if (!shader.parameters.empty()) {
                Json parameters = Json::array();

                for (const ShaderParameter& p : shader.parameters) {
                    Json parameter = Json::object();
                    parameter["name"] = p.name;
                    parameter["min"] = (double)p.minimum;
                    parameter["max"] = (double)p.maximum;
                    parameter["default"] = (double)p.defaultValue;
                    parameters.push_back(parameter);
                }

                s["parameters"] = parameters;
            }

            list.push_back(s);
        };

        for (const ShaderDef& shader : builtinShaders()) add(shader);
        for (const ShaderDef& shader : _userShaders) add(shader);

        return done(list);
    }

    // ------------------------------------------------------------------------
    if (name == "import_media") {
        if (attempt == 0) {
            _mcpImported.clear();

            for (const Json& path : args["paths"].items()) {
                const std::string text = path.toString();
                const std::filesystem::path file(std::u8string(text.begin(), text.end()));
                std::error_code ec;

                if (!std::filesystem::is_regular_file(file, ec)) {
                    return error("No file " + text);
                }

                _mcpImported.push_back(_project.importMedia(file));
            }

            edited("import media");
            return std::nullopt;
        }

        // Analyzed (length, size), or given up after about a minute.
        bool analyzed = true;

        for (Id id : _mcpImported) {
            if (const MediaItem* item = _project.findMedia(id); item && !item->probed) analyzed = false;
        }

        if (!analyzed && attempt < 3600) {
            return std::nullopt;
        }

        Json media = Json::array();

        for (Id id : _mcpImported) {
            if (const MediaItem* item = _project.findMedia(id)) media.push_back(mediaSummary(*item));
        }

        return done(media);
    }

    // ------------------------------------------------------------------------
    if (name == "create_sequence") {
        const Id id = _project.addSequence(args["name"].toString("Sequence " + std::to_string(_project.sequences().size() + 1)));
        showSequence(id);
        Sequence& created = _project.sequence();

        if (args.contains("width") && args.contains("height")) {
            created.width = (uint32_t)std::clamp<int64_t>(args["width"].toInt(), 16, 8192) & ~1u;
            created.height = (uint32_t)std::clamp<int64_t>(args["height"].toInt(), 16, 8192) & ~1u;
        }

        if (args.contains("fps")) created.frameRate = rateOf(args["fps"].toDouble(60.0));

        edited("create sequence");

        Json answer = Json::object();
        answer["sequence_id"] = id;
        return done(answer);
    }

    if (name == "switch_sequence") {
        const Id id = (Id)args["sequence_id"].toInt();

        if (!_project.findSequence(id)) {
            return error("No sequence " + std::to_string(id));
        }

        showSequence(id);
        return McpServer::textResult("Sequence shown.");
    }

    if (name == "update_sequence") {
        if (args.contains("name")) sequence.name = args["name"].toString();

        if (args.contains("width") || args.contains("height")) {
            sequence.resize((uint32_t)std::clamp<int64_t>(args["width"].toInt(sequence.width), 16, 8192) & ~1u,
                            (uint32_t)std::clamp<int64_t>(args["height"].toInt(sequence.height), 16, 8192) & ~1u);
        }

        if (args.contains("fps")) sequence.frameRate = rateOf(args["fps"].toDouble(60.0));

        edited("change sequence");
        return McpServer::textResult("Sequence changed.");
    }

    if (name == "add_track") {
        const std::string type = args["type"].toString();
        const TrackType trackType = (type == "video") ? TrackType::Video : (type == "subtitle") ? TrackType::Subtitle : TrackType::Audio;
        const Id id = _project.newId();
        sequence.addTrack(trackType, id, _project.newId());
        edited("add track");

        Json answer = Json::object();
        answer["track_id"] = id;
        return done(answer);
    }

    // ------------------------------------------------------------------------
    if (name == "add_clip") {
        const Id mediaId = (Id)args["media_id"].toInt();
        const MediaItem* item = _project.findMedia(mediaId);
        Track* track = trackFromArgs("track_id");

        if (!item || !track) {
            return error("No such media or track.");
        }

        const Id clipId = _project.addClip(mediaId, track->id, ticks(args["start"]), args["overwrite"].toBool(false));

        if (clipId == InvalidId) {
            return error("No room there (overwrite: true to replace what is there).");
        }

        // The part of the media used, for the clip and its linked ones.
        Clip* clip = _project.findClip(clipId);
        const Time sourceIn = std::clamp<Time>(ticks(args["source_in"]), 0, std::max<Time>(0, item->duration - 1));
        const Time available = item->type == MediaType::Image ? clip->duration : item->duration - sourceIn;
        const Time duration = args.contains("duration") ? std::clamp<Time>(ticks(args["duration"]), 1, std::max<Time>(1, available)) : std::min(clip->duration, available);
        std::vector<Id> ids = { clipId };

        for (const TrackType type : { TrackType::Video, TrackType::Sound, TrackType::Audio }) {
            for (Track& t : sequence.tracks(type)) {
                for (Clip& c : t.clips) {
                    if (c.id == clipId || (clip->linkGroup != InvalidId && c.linkGroup == clip->linkGroup)) {
                        c.sourceIn = item->type == MediaType::Image ? 0 : sourceIn;
                        c.duration = duration;
                        if (c.id != clipId) ids.push_back(c.id);
                    }
                }
            }
        }

        edited("add clip");

        Json answer = Json::object();
        Json list = Json::array();
        for (Id id : ids) list.push_back(id);
        answer["clip_ids"] = list;
        return done(answer);
    }

    // ------------------------------------------------------------------------
    if (name == "add_text") {
        Track* track = trackFromArgs("track_id");

        if (!track) {
            return error("No such track.");
        }

        const Id id = _project.addTextClip(track->id, ticks(args["start"]), std::max<Time>(1, ticks(args["duration"], 3.0)), args["text"].toString());

        if (id == InvalidId) {
            return error("No room there for the text.");
        }

        Clip* clip = _project.findClip(id);

        for (TextSpan& span : clip->text.spans) {
            if (args.contains("size")) span.style.size = (float)args["size"].toDouble(96.0);

            Color color;

            if (parseColor(args["color"].toString(), color)) {
                span.style.color = color;
            }

            if (args["bold"].toBool(false)) {
                span.style.font = _fonts->variant(span.style.font, "Bold");
            }
        }

        const std::string align = args["align"].toString("center");
        clip->text.align = (align == "left") ? TextAlign::Left : (align == "right") ? TextAlign::Right : TextAlign::Center;

        if (args.contains("x")) clip->transform.x = (float)args["x"].toDouble();
        if (args.contains("y")) clip->transform.y = (float)args["y"].toDouble();

        edited("add text");

        Json answer = Json::object();
        answer["clip_id"] = id;
        return done(answer);
    }

    if (name == "add_title") {
        const std::string preset = args["preset"].toString();
        _playhead = std::max<Time>(0, ticks(args["start"]));

        if (preset == "end_credits") addTitle(TitlePreset::EndCredits);
        else if (preset == "lower_third") addTitle(TitlePreset::LowerThird);
        else if (preset == "news_bar") addTitle(TitlePreset::NewsBar);
        else return error("Unknown preset " + preset);

        Json answer = Json::object();
        Json list = Json::array();
        for (Id id : _selection) list.push_back(id);
        answer["clip_ids"] = list;
        return done(answer);
    }

    if (name == "add_subtitle") {
        if (sequence.subtitleTracks.empty()) {
            sequence.addTrack(TrackType::Subtitle, _project.newId(), _project.newId());
        }

        const Id id = _project.addTextClip(sequence.subtitleTracks.front().id, ticks(args["start"]), std::max<Time>(1, ticks(args["duration"], 2.0)),
            args["text"].toString());

        if (id == InvalidId) {
            return error("Another subtitle is there.");
        }

        edited("add subtitle");

        Json answer = Json::object();
        answer["clip_id"] = id;
        return done(answer);
    }

    if (name == "add_generator") {
        const std::string shaderId = args["shader_id"].toString();
        const ShaderDef* shader = _project.findShader(shaderId);

        for (const ShaderDef& user : _userShaders) {
            if (!shader && user.id == shaderId) shader = &user;
        }

        Track* track = trackFromArgs("track_id");

        if (!shader || shader->kind != ShaderKind::Generator || !track) {
            return error("No such generator shader or track.");
        }

        const ShaderDef copy = *shader;
        const Id id = _project.addGeneratorClip(track->id, ticks(args["start"]), std::max<Time>(1, ticks(args["duration"], 5.0)), copy);

        if (id == InvalidId) {
            return error("No room there.");
        }

        Clip* clip = _project.findClip(id);

        for (size_t i = 0; i < copy.parameters.size() && i < clip->shaderParameters.size(); i++) {
            const Json& value = args["parameters"][copy.parameters[i].name];
            if (value.isNumber()) clip->shaderParameters[i] = AnimatedValue((float)value.toDouble());
        }

        edited("add generator");

        Json answer = Json::object();
        answer["clip_id"] = id;
        return done(answer);
    }

    // ------------------------------------------------------------------------
    if (name == "update_clip") {
        const Id id = (Id)args["clip_id"].toInt();
        Json clip = _project.clipJson(id);

        if (clip.isNull()) {
            return error("No clip " + std::to_string(id) + " in the current sequence.");
        }

        // Objects merged into those of the clip, the rest replaced.
        std::function<void(Json&, const Json&)> merge = [&](Json& into, const Json& changes) {
            for (const auto& [key, value] : changes.members()) {
                if (value.isObject() && into[key].isObject()) merge(into[key], value);
                else into[key] = value;
            }
        };

        merge(clip, args["changes"]);
        std::string failure;

        if (!_project.setClipJson(id, clip, args["overwrite"].toBool(false), failure)) {
            return error(failure);
        }

        edited("change clip");

        // The fields that did not make it (unknown, or not for this kind).
        const Json result = _project.clipJson(id);
        std::string ignored;
        std::function<void(const Json&, const Json&, const std::string&)> check = [&](const Json& changes, const Json& now, const std::string& prefix) {
            for (const auto& [key, value] : changes.members()) {
                if (!now.contains(key)) ignored += (ignored.empty() ? "" : ", ") + prefix + key;
                else if (value.isObject() && now[key].isObject()) check(value, now[key], prefix + key + ".");
            }
        };

        check(args["changes"], result, "");

        Json answer = Json::object();
        answer["clip"] = result;
        if (!ignored.empty()) answer["ignoredFields"] = ignored;
        return done(answer);
    }

    if (name == "move_clip") {
        const Id id = (Id)args["clip_id"].toInt();
        const Clip* clip = _project.findClip(id);
        Track* from = _project.findTrackOfClip(id);

        if (!clip || !from) {
            return error("No clip " + std::to_string(id));
        }

        Track* to = args.contains("track_id") ? _project.findTrack((Id)args["track_id"].toInt()) : from;

        if (!to || to->type != from->type) {
            return error("The track must be of the same kind as the clip's.");
        }

        const Time delta = std::max<Time>(0, ticks(args["start"])) - clip->start;
        const bool overwrite = args["overwrite"].toBool(false);
        const Id linkGroup = args["with_links"].toBool(true) ? clip->linkGroup : InvalidId;
        const Id toId = to->id;
        Project before = _project;

        // Taken off their tracks, then placed again.
        struct Moved { Clip clip; Id track; };
        std::vector<Moved> moved;

        for (const TrackType type : { TrackType::Video, TrackType::Sound, TrackType::Audio, TrackType::Subtitle }) {
            for (Track& t : sequence.tracks(type)) {
                for (const Clip& c : t.clips) {
                    if (c.id == id || (linkGroup != InvalidId && c.linkGroup == linkGroup)) {
                        moved.push_back({ c, c.id == id ? toId : t.id });
                    }
                }

                std::erase_if(t.clips, [&](const Clip& c) { return c.id == id || (linkGroup != InvalidId && c.linkGroup == linkGroup); });
            }
        }

        for (Moved& m : moved) {
            m.clip.start = std::max<Time>(0, m.clip.start + delta);
            Track* track = _project.findTrack(m.track);

            const bool blocked = std::any_of(track->clips.begin(), track->clips.end(), [&](const Clip& c) {
                return c.start < m.clip.end() && m.clip.start < c.end();
            });

            if (blocked && !overwrite) {
                _project = std::move(before);
                return error("Clips are in the way (overwrite: true to replace them).");
            }

            if (blocked) {
                _project.overwrite(m.track, m.clip.start, m.clip.end());
                track = _project.findTrack(m.track);
            }

            track->clips.push_back(m.clip);
            track->sortClips();
        }

        edited("move clip");
        return McpServer::textResult("Moved.");
    }

    if (name == "split_clips") {
        std::vector<Id> ids;
        for (const Json& id : args["clip_ids"].items()) ids.push_back((Id)id.toInt());

        _project.splitAt(snapToFrame(ticks(args["time"]), sequence.frameRate), ids);
        edited("split clips");
        return McpServer::textResult("Split. get_project gives the new clips.");
    }

    if (name == "delete_clips") {
        std::vector<Id> ids;
        for (const Json& id : args["clip_ids"].items()) ids.push_back((Id)id.toInt());

        const bool links = args["with_links"].toBool(true);

        if (args["ripple"].toBool(false)) {
            _project.rippleDelete(ids, links);
        }
        else {
            _project.removeClips(ids, links);
        }

        std::erase_if(_selection, [&](Id id) { return !_project.findClip(id); });
        edited("delete clips");
        return McpServer::textResult("Deleted.");
    }

    if (name == "set_transition") {
        Clip* clip = _project.findClip((Id)args["clip_id"].toInt());

        if (!clip) {
            return error("No such clip.");
        }

        const std::string type = args["type"].toString();
        const TransitionName* found = nullptr;

        for (const TransitionName& t : TransitionNames) {
            if (type == t.name) found = &t;
        }

        if (!found) {
            return error("Unknown transition " + type);
        }

        clip->transitionIn.type = found->type;
        clip->transitionIn.duration = std::max<Time>(1, ticks(args["duration"], 1.0));

        if (found->type == TransitionType::Shader) {
            const std::string shaderId = args["shader_id"].toString();
            const ShaderDef* shader = _project.findShader(shaderId);

            for (const ShaderDef& user : _userShaders) {
                if (!shader && user.id == shaderId) {
                    _project.storeShader(user);
                    shader = _project.findShader(shaderId);
                }
            }

            if (!shader || shader->kind != ShaderKind::Transition) {
                return error("No transition shader " + shaderId);
            }

            clip->transitionIn.shader = shaderId;
        }

        edited("set transition");
        return McpServer::textResult("Transition set.");
    }

    if (name == "add_effect") {
        const std::string type = args["type"].toString();
        const VideoEffectInfo* info = videoEffectInfo(type);

        if (!info) {
            return error("Unknown effect " + type);
        }

        std::vector<VideoEffect>* effects = nullptr;

        if (Clip* clip = args.contains("clip_id") ? _project.findClip((Id)args["clip_id"].toInt()) : nullptr) {
            effects = &clip->videoEffects;
        }
        else if (Track* track = trackFromArgs("track_id"); track && track->type == TrackType::Video) {
            effects = &track->videoEffects;
        }

        if (!effects) {
            return error("Give a clip_id or the track_id of a video track.");
        }

        VideoEffect effect = makeVideoEffect(_project.newId(), type);

        for (size_t i = 0; i < info->parameters.size() && i < effect.parameters.size(); i++) {
            const Json& value = args["parameters"][info->parameters[i].name];
            if (value.isNumber()) effect.parameters[i] = AnimatedValue((float)value.toDouble());
        }

        effects->push_back(effect);
        edited("add effect");

        Json answer = Json::object();
        answer["effect_id"] = effect.id;
        return done(answer);
    }

    // ------------------------------------------------------------------------
    if (name == "write_shader") {
        std::string shaderName = args["name"].toString();
        std::erase_if(shaderName, [](char c) { return std::string("\\/:*?\"<>|").find(c) != std::string::npos; });

        if (shaderName.empty()) {
            return error("A name is needed.");
        }

        ShaderDef shader;

        if (!parseShader(args["source"].toString(), shader)) {
            return error("The shader defines none of generate(), transition(), blend(), resample().");
        }

        shader.name = shaderName;
        shader.id = "file:" + shaderName;

        if (const std::string compileError = Compositor::checkShader(shader); !compileError.empty()) {
            return error("It does not compile:\n" + compileError);
        }

        std::error_code ec;
        std::filesystem::create_directories(shaderFolder(), ec);
        const std::filesystem::path path = shaderFolder() / std::filesystem::u8path(shaderName + ".glsl");

        if (!(std::ofstream(path, std::ios::binary) << shader.source)) {
            return error("Could not write the file.");
        }

        reloadShaders(true);

        Json answer = Json::object();
        answer["shader_id"] = shader.id;
        answer["kind"] = shaderKindName(shader.kind);
        return done(answer);
    }

    // ------------------------------------------------------------------------
    if (name == "set_playhead") {
        stopPlayback();
        seek(std::max<Time>(0, ticks(args["time"])));
        return McpServer::textResult("Playhead moved.");
    }

    if (name == "undo") {
        undo();
        return McpServer::textResult("Undone.");
    }

    if (name == "redo") {
        redo();
        return McpServer::textResult("Redone.");
    }

    if (name == "save_project") {
        const std::string text = args["path"].toString();
        const std::filesystem::path path = text.empty() ? _projectPath : std::filesystem::path(std::u8string(text.begin(), text.end()));

        if (path.empty()) {
            return error("The project was never saved: give a path.");
        }

        return saveProject(path) ? McpServer::textResult("Saved.") : error("Could not save the project.");
    }

    if (name == "export_video") {
        if (attempt == 0) {
            if (_exporter && !_exporter->done()) {
                return error("An export is already running.");
            }

            _exporter.reset();

            const std::string preset = args["preset"].toString();

            for (int i = 0; i < (int)exportPresetCount(); i++) {
                if (preset == exportPresetName(i)) applyExportPreset(i);
            }

            const std::string codec = args["codec"].toString();
            if (codec == "h264") _exportSettings.codec = ExportSettings::Codec::H264;
            if (codec == "h265") _exportSettings.codec = ExportSettings::Codec::H265;
            if (codec == "av1") _exportSettings.codec = ExportSettings::Codec::AV1;
            if (args.contains("height")) _exportSettings.height = (uint32_t)std::max<int64_t>(0, args["height"].toInt()) & ~1u;
            if (args.contains("bitrate_mbps")) _exportSettings.bitrateMbps = (float)args["bitrate_mbps"].toDouble(16.0);

            const std::string path = args["path"].toString();
            _exportSettings.path = std::filesystem::path(std::u8string(path.begin(), path.end()));

            if (mediaInAnalysis() > 0) {
                return std::nullopt;
            }

            if (!startExport()) {
                return error("The export could not start (an empty sequence?).");
            }

            return std::nullopt;
        }

        if (!_exporter) {
            // Waiting for the media to be analyzed.
            if (mediaInAnalysis() > 0) {
                return std::nullopt;
            }

            return startExport() ? Result() : error("The export could not start.");
        }

        if (!_exporter->done()) {
            return std::nullopt;
        }

        const bool succeeded = _exporter->succeeded();
        const std::string message = succeeded ? "Exported to " + _exportSettings.path.string() + " (encoder " + _exporter->encoder() + ")."
                                              : "The export failed: " + _exporter->error();
        _exporter.reset();
        return McpServer::textResult(message, !succeeded);
    }

    return error("Unknown tool " + name);
}

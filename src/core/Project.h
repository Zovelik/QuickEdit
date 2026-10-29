#pragma once

#include <core/Animation.h>
#include <core/Json.h>
#include <core/Text.h>
#include <core/Time.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>


namespace core
{
    // Identifiers are unique within a project and never reused, so that the
    // undo history and the selection can refer to objects that were deleted
    // and restored.
    using Id = uint64_t;
    constexpr Id InvalidId = 0;


    // Length given to the still images.
    constexpr Time DefaultImageDuration = 5 * TicksPerSecond;


    enum class MediaType
    {
        Video,      // Video, with or without audio
        Audio,
        Image
    };


    // A file imported in the project. The stream properties are filled when
    // the file is probed by the decoder.
    struct MediaItem
    {
        Id id = InvalidId;
        std::filesystem::path path;
        MediaType type = MediaType::Video;

        // False until the file was probed: the properties below are then
        // placeholders.
        bool probed = false;

        Time duration = 0;
        uint32_t width = 0;
        uint32_t height = 0;
        Rational frameRate = { 30, 1 };
        bool hasAudio = false;
        uint32_t sampleRate = 0;
        uint32_t channels = 0;

        std::string name() const;
    };


    enum class ClipKind
    {
        Media,      // A piece of a media item
        Text        // A text, generated
    };


    // Placement of a video clip in the frame of the sequence, animatable.
    struct Transform
    {
        AnimatedValue x = 0.f;          // Offset of the center, in pixels
        AnimatedValue y = 0.f;
        AnimatedValue scale = 1.f;
        AnimatedValue rotation = 0.f;   // Degrees, clockwise
        AnimatedValue opacity = 1.f;    // [0, 1]

        void shiftKeys(Time offset);
    };


    enum class TransitionType
    {
        None,
        Dissolve,       // Cross fade
        DipToBlack,     // Fade out, then in
        Push,           // The incoming picture pushes the other one left
        Wipe,           // The incoming picture is revealed from the left
        Zoom            // The incoming picture grows and fades in
    };


    // Transition from the clip before (on the same track) into a clip, over
    // duration centered on the cut.
    struct Transition
    {
        TransitionType type = TransitionType::None;
        Time duration = TicksPerSecond;
    };


    // An element of a track: a piece of a media item, or a text.
    // Look of the picture of a video or image clip.
    struct Frame
    {
        // Parts cut from each edge, as fractions of the picture: the rest
        // stays where it was.
        float cropLeft = 0.f;
        float cropRight = 0.f;
        float cropTop = 0.f;
        float cropBottom = 0.f;

        float cornerRadius = 0.f;       // Pixels

        float borderWidth = 0.f;        // Pixels, 0 for none
        Color borderColor;

        float shadowOffsetX = 0.f;      // Pixels, all 0 for none
        float shadowOffsetY = 0.f;
        float shadowSoftness = 0.f;     // Pixels of blur
        Color shadowColor = { 0.f, 0.f, 0.f, 0.6f };

        bool hasShadow() const
        {
            return shadowColor.a > 0.f && (shadowOffsetX != 0.f || shadowOffsetY != 0.f || shadowSoftness > 0.f);
        }

        bool operator==(const Frame& other) const = default;
    };


    struct Clip
    {
        Id id = InvalidId;
        ClipKind kind = ClipKind::Media;
        Id mediaId = InvalidId;

        // Position on the timeline and length of the clip.
        Time start = 0;
        Time duration = 0;

        // Time in the media matching the start of the clip.
        Time sourceIn = 0;

        // Clips of the same link group (the video and the audio of a file)
        // are selected, moved and trimmed together.
        Id linkGroup = InvalidId;

        // Video: placement in the frame. Audio: gain in dB, and fades at the
        // edges of the clip.
        Transform transform;
        float gainDb = 0.f;
        Time fadeIn = 0;
        Time fadeOut = 0;

        // Shows the frame at sourceIn for the whole clip (freeze frame).
        bool frozen = false;

        // Text clips only.
        TextContent text;

        // Video clips: transition from the previous clip.
        Transition transitionIn;

        // Video and image clips: crop, border and shadow.
        Frame frame;

        Time end() const { return start + duration; }
    };


    enum class TrackType
    {
        Video,
        Sound,      // The sound of the clips of the video track of the same index
        Audio       // Voice, music...
    };


    // An audio effect (VST3 plugin) of a track.
    struct EffectSlot
    {
        Id id = InvalidId;
        std::string path;           // Of the .vst3 bundle or file, UTF-8
        std::string classId;        // Of the plugin class, in hexadecimal
        std::string name;
        bool bypass = false;

        // State of the plugin, base64 (component, then controller).
        std::string state;
    };


    struct Track
    {
        Id id = InvalidId;
        TrackType type = TrackType::Video;
        std::string name;

        bool muted = false;     // Video: hidden. Audio: muted.
        bool solo = false;      // Audio only
        bool locked = false;

        // Clips sorted by start time, never overlapping.
        std::vector<Clip> clips;

        // Audio tracks: effects, in processing order.
        std::vector<EffectSlot> effects;

        // Index of the clip at time t, or -1.
        int clipAt(Time t) const;

        // Sorts the clips by start time.
        void sortClips();
    };


    // An edit: the tracks and their clips, with the output format.
    struct Sequence
    {
        std::string name = "Sequence 1";

        uint32_t width = 1920;
        uint32_t height = 1080;
        Rational frameRate = { 60, 1 };
        uint32_t sampleRate = 48000;

        // Video tracks are composited from the last (bottom) to the first
        // (top), as shown in the timeline. Each one has a sound track, of the
        // same index, for the sound of its video clips. Audio tracks hold
        // the other sounds (voice, music).
        std::vector<Track> videoTracks;
        std::vector<Track> soundTracks;
        std::vector<Track> audioTracks;

        // The sound tracks are folded in the timeline.
        bool soundTracksFolded = false;

        std::vector<Track>& tracks(TrackType type)
        {
            return (type == TrackType::Video) ? videoTracks : (type == TrackType::Sound ? soundTracks : audioTracks);
        }

        const std::vector<Track>& tracks(TrackType type) const
        {
            return const_cast<Sequence*>(this)->tracks(type);
        }

        // Adds a video track with its sound track, or an audio track.
        void addTrack(TrackType type, Id id, Id soundId);

        // End of the last clip.
        Time duration() const;
    };


    class Project
    {
    public:
        // Format version written in the project files. Older versions are
        // upgraded on load, newer ones are rejected.
        static constexpr int FileVersion = 1;
        static constexpr const char* FileExtension = "qep";

        Project();

        Id newId() { return _nextId++; }

        std::vector<MediaItem>& media() { return _media; }
        const std::vector<MediaItem>& media() const { return _media; }

        Sequence& sequence() { return _sequence; }
        const Sequence& sequence() const { return _sequence; }

        MediaItem* findMedia(Id id);
        const MediaItem* findMedia(Id id) const;

        // Track holding the given clip, or nullptr.
        Track* findTrackOfClip(Id clipId);
        Clip* findClip(Id clipId);
        const Clip* findClip(Id clipId) const { return const_cast<Project*>(this)->findClip(clipId); }
        Track* findTrack(Id trackId);
        EffectSlot* findEffect(Id slotId);

        // Imports a file. Returns the id of the new media item, or the id of
        // the existing one when the file was already imported.
        Id importMedia(const std::filesystem::path& path);

        // Places the given media on a track at time start, with its audio on
        // the first audio track when the media has some. Returns the id of
        // the video (or audio) clip, InvalidId when there is no room.
        Id addClip(Id mediaId, Id trackId, Time start);

        // Removes the clips, and their linked clips when withLinks (a
        // selection already holds them, unless picked alone).
        void removeClips(const std::vector<Id>& clipIds, bool withLinks = true);

        // Removes the clips and their linked clips, and closes the gaps they
        // leave: the next clips of their tracks move back by the removed
        // length. Other tracks are left as they are.
        void rippleDelete(const std::vector<Id>& clipIds, bool withLinks = true);

        // Removes the empty range [start, end) of a track: its clips after
        // it move back. The paired video / sound track moves too when it is
        // empty there, so that the videos keep their sound. False when the
        // range is not empty.
        bool closeGap(Id trackId, Time start, Time end);

        // Cuts every clip crossing time t on unlocked tracks. Only the
        // selected clips (and their links) when the selection is not empty.
        void splitAt(Time t, const std::vector<Id>& onlyClips = {});

        // Places a text on a video track. Returns its id, InvalidId when there
        // is no room at start.
        Id addTextClip(Id trackId, Time start, Time duration, const std::string& text);

        // Freezes the frame of a video clip shown at time t for the given
        // duration: the clip is cut at t, a still of that frame is inserted
        // and the rest of the clip, with the clips after it on its track and
        // on the tracks of its linked clips, moves later by duration.
        // Returns the id of the still clip, InvalidId when t is not in the clip.
        Id insertFreezeFrame(Id clipId, Time t, Time duration);

        // Free range around t on the track, excluding the clip ignoreClip:
        // [freeStart, freeEnd). Returns false when t is inside a clip.
        bool freeRangeAt(const Track& track, Time t, Id ignoreClip, Time& freeStart, Time& freeEnd) const;

        Json toJson(const std::filesystem::path& projectDir) const;
        void fromJson(const Json& json, const std::filesystem::path& projectDir);

        void save(const std::filesystem::path& filepath) const;
        void load(const std::filesystem::path& filepath);

    private:
        Id _nextId = 1;

        std::vector<MediaItem> _media;
        Sequence _sequence;
    };


    MediaType mediaTypeFromExtension(const std::filesystem::path& path);
}

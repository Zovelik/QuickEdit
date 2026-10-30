#pragma once

#include <core/Animation.h>
#include <core/Json.h>
#include <core/Shaders.h>
#include <core/Text.h>
#include <core/Time.h>
#include <core/VideoEffects.h>

#include <algorithm>
#include <cmath>
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

        // Folder of the media panel ('/' between the levels), empty for none.
        std::string folder;

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

        // The audio streams of the file (their titles, may be empty): a
        // screen recording has the computer sound and the microphone.
        std::vector<std::string> audioStreams;

        std::string name() const;

        // "Stream 2", or its title.
        std::string audioStreamName(int stream) const;
    };


    enum class ClipKind
    {
        Media,      // A piece of a media item
        Text,       // A text, generated
        Generator,  // A picture made by a shader (core/Shaders.h)
        Sequence    // Another sequence of the project, nested
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
        Zoom,           // The incoming picture grows and fades in
        Shader          // A transition shader (core/Shaders.h)
    };


    // Transition from the clip before (on the same track) into a clip, over
    // duration centered on the cut.
    struct Transition
    {
        TransitionType type = TransitionType::None;
        Time duration = TicksPerSecond;
        std::string shader;         // Id of a shader, for TransitionType::Shader
    };


    // An element of a track: a piece of a media item, or a text.
    // An audio effect (VST3 plugin) of a track or a clip.
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


    // Size of a picture in the frame of the sequence, before the scale of its
    // transform.
    enum class FitMode
    {
        Fit,        // Whole, as large as the frame allows
        Fill,       // Covering the whole frame, cut at the edges
        Native      // One pixel of the picture per pixel of the sequence
    };

    const char* fitModeName(FitMode mode);


    // How a picture is mixed with the pictures below it.
    enum class BlendMode
    {
        Normal,
        Add,
        Screen,
        Multiply,
        Lighten,
        Darken,
        Count
    };

    const char* blendModeName(BlendMode mode);
    BlendMode blendModeFromName(const std::string& name);


    // Sampling of a picture drawn at another size.
    enum class ScaleFilter
    {
        Bilinear,   // Smooth
        Nearest     // Sharp pixels (screen captures at whole scales)
    };

    // Sequence pixels per pixel of a picture of the given size, in a frame
    // of the given size.
    float fitScale(FitMode mode, float frameWidth, float frameHeight, float pictureWidth, float pictureHeight);


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

        // A sound or audio clip not heard (an audio stream of a file, kept
        // aside).
        bool muted = false;

        // A sound clip of all the streams of a file (audioStream -1): the
        // streams muted, and the ones removed (no longer under its track).
        std::vector<int> mutedStreams;
        std::vector<int> removedStreams;

        bool streamMuted(int stream) const { return std::find(mutedStreams.begin(), mutedStreams.end(), stream) != mutedStreams.end(); }
        bool streamRemoved(int stream) const { return std::find(removedStreams.begin(), removedStreams.end(), stream) != removedStreams.end(); }

        // The audio heard: audioStream, -1 for all the streams, or the
        // streams heard as -2 - (the mask of the others), the first 30
        // (see media::MediaManager::audio()).
        int audioSource() const
        {
            if (audioStream >= 0 || (mutedStreams.empty() && removedStreams.empty())) {
                return audioStream;
            }

            int excluded = 0;

            for (const std::vector<int>* streams : { &mutedStreams, &removedStreams }) {
                for (int stream : *streams) {
                    if (stream >= 0 && stream < 30) excluded |= 1 << stream;
                }
            }

            return -2 - excluded;
        }

        // Media clips: playback speed, 2 shows the media twice as fast (its
        // sound higher). The clip shows duration * speed of the media.
        double speed = 1.0;

        // Sound clips: the audio stream of the media heard, -1 for all of
        // them together.
        int audioStream = -1;

        // Text clips only.
        TextContent text;

        // Generators: the shader, and the values of its parameters.
        std::string shader;
        std::vector<AnimatedValue> shaderParameters;

        // Nested sequences: the sequence, shown from sourceIn (its picture,
        // and its sound mixed with the sound track of the video track).
        Id sequence = InvalidId;

        // Video clips: transition from the previous clip.
        Transition transitionIn;

        // Video and image clips: size in the frame, sampling, mixing with
        // the pictures below (Normal: the one of the track), crop, border
        // and shadow.
        FitMode fit = FitMode::Fit;
        ScaleFilter filter = ScaleFilter::Bilinear;
        BlendMode blend = BlendMode::Normal;
        Frame frame;

        // Effects of the clip, before those of its track: on its picture
        // (video), or on its sound (audio, in processing order).
        std::vector<VideoEffect> videoEffects;
        std::vector<EffectSlot> effects;

        Time end() const { return start + duration; }

        // A length of the clip as a length of its media, and back.
        Time toSource(Time t) const { return (speed == 1.0) ? t : (Time)std::llround((double)t * speed); }
        Time fromSource(Time t) const { return (speed == 1.0) ? t : (Time)std::llround((double)t / speed); }

        // Time of the media shown at time localTime of the clip.
        Time sourceAt(Time localTime) const { return frozen ? sourceIn : sourceIn + toSource(localTime); }
    };


    enum class TrackType
    {
        Video,
        Sound,      // The sound of the clips of the video track of the same index
        Audio,      // Voice, music...
        Subtitle    // Text clips shown as subtitles (core/Subtitles.h)
    };


    // Look of the subtitles of a sequence.
    struct SubtitleStyle
    {
        float size = 54.f;              // Pixels of the sequence
        Color color;
        float outlineWidth = 4.f;
        float bottomMargin = 70.f;      // From the bottom of the frame

        bool operator==(const SubtitleStyle& other) const = default;
    };


    struct Track
    {
        Id id = InvalidId;
        TrackType type = TrackType::Video;
        std::string name;

        bool muted = false;     // Video: hidden. Audio: muted.
        bool solo = false;      // Audio only
        bool locked = false;

        // Sound and audio tracks: the audio streams of their clips shown
        // under them, one row each.
        bool streamsUnfolded = false;

        // Height in the timeline, in font sizes; 0 for the default one.
        float height = 0.f;

        // Video tracks: mixing of their clips with the tracks below (a clip
        // can have its own).
        BlendMode blend = BlendMode::Normal;

        // Clips sorted by start time, never overlapping.
        std::vector<Clip> clips;

        // Audio tracks: effects, in processing order. Video tracks: effects
        // on the picture of each of their clips, after the clip's own.
        std::vector<EffectSlot> effects;
        std::vector<VideoEffect> videoEffects;

        // Index of the clip at time t, or -1.
        int clipAt(Time t) const;

        // Sorts the clips by start time.
        void sortClips();
    };


    // An edit: the tracks and their clips, with the output format.
    struct Sequence
    {
        Id id = InvalidId;
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
        std::vector<Track> subtitleTracks;
        SubtitleStyle subtitleStyle;
        std::vector<Track> audioTracks;

        // The sound tracks are folded in the timeline.
        bool soundTracksFolded = false;

        std::vector<Track>& tracks(TrackType type)
        {
            return (type == TrackType::Video) ? videoTracks
                : (type == TrackType::Sound) ? soundTracks
                : (type == TrackType::Subtitle) ? subtitleTracks : audioTracks;
        }

        const std::vector<Track>& tracks(TrackType type) const
        {
            return const_cast<Sequence*>(this)->tracks(type);
        }

        // Adds a video track with its sound track, an audio track, or a
        // subtitle track.
        void addTrack(TrackType type, Id id, Id soundId);

        // Removes a track with its clips: a video track with its sound
        // track (and the other way around). The tracks keeping a default
        // name (V2, A3...) are renamed after their new place. False when it
        // is the last video track, or not a track of the sequence.
        bool removeTrack(Id trackId);

        // The track of the given id, or nullptr.
        Track* findTrack(Id trackId);
        const Track* findTrack(Id trackId) const { return const_cast<Sequence*>(this)->findTrack(trackId); }

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

        // Folders of the media panel, '/' between the levels, empty ones
        // included.
        const std::vector<std::string>& mediaFolders() const { return _mediaFolders; }

        // A new folder in parent (empty: at the top), its name made unique.
        // Returns its path.
        std::string addMediaFolder(const std::string& parent, const std::string& name);

        // Renames the last level of a folder, its subfolders and media
        // following. False when the name is empty, has a '/', or is taken.
        bool renameMediaFolder(const std::string& path, const std::string& name);

        // Removes a folder and its subfolders; their media go to its parent.
        void removeMediaFolder(const std::string& path);

        // Moves media into a folder (empty: out of any).
        void moveMediaToFolder(Id mediaId, const std::string& folder);

        static std::string parentFolder(const std::string& path);

        // The sequence being edited; the others are kept apart until shown.
        Sequence& sequence() { return _sequence; }
        const Sequence& sequence() const { return _sequence; }

        // All the sequences, in the order of their tabs.
        std::vector<const Sequence*> sequences() const;
        std::vector<Sequence*> sequences();
        const Sequence* findSequence(Id id) const;

        // A new sequence with the settings of the current one and empty
        // tracks, after the others. Returns its id.
        Id addSequence(const std::string& name);

        // Edits another sequence. The selections refer to clips of the
        // current one: clear them.
        bool switchSequence(Id id);

        // False when it is the only one, or nested in another.
        bool removeSequence(Id id);

        // True when outer is inner, or shows it (nested at any depth).
        bool sequenceContains(Id outer, Id inner) const;

        // A clip of another sequence on a video track of the current one, as
        // long as the sequence (not containing the current one).
        Id addSequenceClip(Id sequenceId, Id trackId, Time start);

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
        // overwrite: the clips in the way are shortened, cut or removed
        // (see overwrite()) rather than the new clip shortened to fit.
        Id addClip(Id mediaId, Id trackId, Time start, bool overwrite = false);

        // The tracks a clip of a media dropped on a track goes on: its video
        // track and its sound track (InvalidId for none of either).
        std::pair<Id, Id> clipTracks(Id mediaId, Id trackId);

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

        // Sets the speed of a media clip and of its linked clips, keeping
        // the part of the media they show: their length changes, and the
        // clips after them on their tracks follow. Speeds are clamped to
        // [MinSpeed, MaxSpeed]. False when nothing changed.
        // A sound clip of a media with several audio streams: one linked clip
        // per stream, the others on audio tracks free over the clip (new
        // ones when needed); or back to a single clip of all the streams.
        // False when nothing changed.
        bool unfoldAudioStreams(Id clipId);
        bool foldAudioStreams(Id clipId);

        static constexpr double MinSpeed = 0.1;
        static constexpr double MaxSpeed = 16.0;
        bool setClipSpeed(Id clipId, double speed);

        // Cuts every clip crossing time t on unlocked tracks. Only the
        // selected clips (and their links) when the selection is not empty.
        void splitAt(Time t, const std::vector<Id>& onlyClips = {});

        // Frees [from, to) on a track for a clip placed over others, except
        // the clips kept: the clips inside go, the ones across an edge are
        // shortened, one across both is cut in two (its parts keep its
        // link group). Its linked clips on other tracks are left alone.
        void overwrite(Id trackId, Time from, Time to, const std::vector<Id>& keep = {});

        // Places a text on a video track. Returns its id, InvalidId when there
        // is no room at start.
        // On a video track, or a subtitle track (a line of subtitles).
        Id addTextClip(Id trackId, Time start, Time duration, const std::string& text);

        // A clip of a generator shader, its parameters at their defaults.
        // InvalidId when there is no room at start on the track.
        Id addGeneratorClip(Id trackId, Time start, Time duration, const ShaderDef& shader);

        // The shaders of the user used in the project, saved with it (the
        // built-in ones come from the code).
        const std::vector<ShaderDef>& shaders() const { return _shaders; }
        void storeShader(const ShaderDef& shader);
        const ShaderDef* findShader(const std::string& id) const;

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
        // The part of a clip from t on, as a new clip (new ids for it and
        // its effects, the same link group); the clip keeps what is before.
        Clip cutClip(Clip& left, Time t);

        Id _nextId = 1;

        std::vector<MediaItem> _media;
        std::vector<std::string> _mediaFolders;
        std::vector<ShaderDef> _shaders;
        Sequence _sequence;
        std::vector<Sequence> _otherSequences;
        std::vector<Id> _sequenceOrder;
    };


    MediaType mediaTypeFromExtension(const std::filesystem::path& path);
}

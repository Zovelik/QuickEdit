// Tests of the core model (time, JSON, project edits). No framework: each
// check prints the failing expression and the test returns a non zero code.
//
// Run: ctest, or bin/QuickEditCoreTests.exe

#include <audio/Mixer.h>
#include <media/MediaFile.h>

#include <core/History.h>
#include <core/Layers.h>
#include <core/TextEdit.h>
#include <core/Json.h>
#include <core/Project.h>
#include <core/Time.h>

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>


using namespace core;


namespace {

int failures = 0;

#define CHECK(expr) \
    do { \
        if (!(expr)) { \
            std::printf("%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #expr); \
            failures++; \
        } \
    } while (0)


void testTime()
{
    const Rational fps30 = { 30, 1 };
    const Rational ntsc = { 30000, 1001 };

    CHECK(frameToTime(30, fps30) == TicksPerSecond);
    CHECK(timeToFrame(TicksPerSecond, fps30) == 30);
    CHECK(timeToFrame(TicksPerSecond - 1, fps30) == 29);
    CHECK(timeToFrame(-1, fps30) == -1);

    // NTSC frames round-trip, also far in a long timeline.
    for (int64_t frame : { 0ll, 1ll, 29ll, 30ll, 1798ll, 107892ll, 10000000ll }) {
        CHECK(timeToFrame(frameToTime(frame, ntsc), ntsc) == frame);
    }

    CHECK(formatTimecode(frameToTime(30 * 3661 + 12, fps30), fps30) == "01:01:01:12");
    CHECK(formatTimecode(0, ntsc) == "00:00:00:00");
}


void testJson()
{
    Json json = Json::object();
    json["int"] = (int64_t)9007199254740993ll;   // Not representable as a double
    json["float"] = 0.1;
    json["text"] = "quote \" backslash \\ newline \n \xc3\xa9";
    json["array"].push_back(1);
    json["array"].push_back(true);
    json["array"].push_back(Json());

    const Json parsed = Json::parse(json.dump());

    CHECK(parsed["int"].toInt() == 9007199254740993ll);
    CHECK(parsed["float"].toDouble() == 0.1);
    CHECK(parsed["text"].toString() == json["text"].toString());
    CHECK(parsed["array"].size() == 3);
    CHECK(parsed["array"][1].toBool());
    CHECK(parsed["array"][2].isNull());
    CHECK(parsed["missing"].toInt(42) == 42);

    CHECK(Json::parse("{\"a\": \"\\u00e9\\ud83d\\ude00\"}")["a"].toString() == "\xc3\xa9\xf0\x9f\x98\x80");

    bool threw = false;
    try { Json::parse("{\"a\": }"); } catch (const std::exception&) { threw = true; }
    CHECK(threw);

    // Copies share their content until one of them is modified.
    Json copy = parsed;
    copy["int"] = 1;
    CHECK(parsed["int"].toInt() == 9007199254740993ll);
}


void testProjectEdits()
{
    Project project;
    Sequence& sequence = project.sequence();

    const Id video = project.importMedia("C:/media/clip.mp4");
    const Id music = project.importMedia("C:/media/music.wav");

    CHECK(project.importMedia("C:/media/clip.mp4") == video);
    CHECK(project.findMedia(music)->type == MediaType::Audio);

    const Id v1 = sequence.videoTracks[0].id;
    const Id a2 = sequence.audioTracks[1].id;

    // A video brings its sound, linked, on the sound track of its video track.
    const Id clip = project.addClip(video, v1, 0);
    CHECK(clip != InvalidId);
    CHECK(sequence.videoTracks[0].clips.size() == 1);
    CHECK(sequence.soundTracks[0].clips.size() == 1);
    CHECK(sequence.audioTracks[0].clips.empty());
    CHECK(sequence.videoTracks[0].clips[0].linkGroup != InvalidId);
    CHECK(sequence.videoTracks[0].clips[0].linkGroup == sequence.soundTracks[0].clips[0].linkGroup);

    // A second video on V2 at the same time: its sound on S2.
    CHECK(project.addClip(video, sequence.videoTracks[1].id, 0) != InvalidId);
    CHECK(sequence.soundTracks[1].clips.size() == 1);
    project.removeClips({ sequence.videoTracks[1].clips[0].id });
    CHECK(sequence.soundTracks[1].clips.empty());

    // No overlap: a clip cannot start inside another one.
    CHECK(project.addClip(video, v1, TicksPerSecond) == InvalidId);

    // A clip is shortened to fit before the next one.
    CHECK(project.addClip(music, a2, 20 * TicksPerSecond) != InvalidId);
    CHECK(project.addClip(music, a2, 15 * TicksPerSecond) != InvalidId);
    CHECK(sequence.audioTracks[1].clips[0].duration == 5 * TicksPerSecond);

    // Split at 4 s: both linked clips are cut, the right parts stay linked.
    project.splitAt(4 * TicksPerSecond, { clip });
    CHECK(sequence.videoTracks[0].clips.size() == 2);
    CHECK(sequence.soundTracks[0].clips.size() == 2);
    CHECK(sequence.audioTracks[1].clips.size() == 2);   // Not selected: untouched

    const Clip& right = sequence.videoTracks[0].clips[1];
    CHECK(right.start == 4 * TicksPerSecond);
    CHECK(right.sourceIn == 4 * TicksPerSecond);
    CHECK(right.linkGroup != sequence.videoTracks[0].clips[0].linkGroup);
    CHECK(right.linkGroup == sequence.soundTracks[0].clips[1].linkGroup);

    // Removing a clip removes its link, unless asked for the clip alone.
    project.removeClips({ sequence.soundTracks[0].clips[1].id }, false);
    CHECK(sequence.videoTracks[0].clips.size() == 2);
    CHECK(sequence.soundTracks[0].clips.size() == 1);

    project.removeClips({ right.id });
    CHECK(sequence.videoTracks[0].clips.size() == 1);
    CHECK(sequence.soundTracks[0].clips.size() == 1);
}


void testSaveLoadAndHistory()
{
    Project project;
    const Id media = project.importMedia(std::filesystem::temp_directory_path() / "QuickEditTests" / "media" / "clip.mov");
    project.addClip(media, project.sequence().videoTracks[1].id, 2 * TicksPerSecond);
    project.sequence().frameRate = { 24000, 1001 };

    History history;
    history.reset(project);
    CHECK(!history.modified());

    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "QuickEditTests";
    std::filesystem::create_directories(dir);
    const std::filesystem::path file = dir / "test.qep";

    project.save(file);

    Project loaded;
    loaded.load(file);

    CHECK(loaded.media().size() == 1);
    CHECK(loaded.media()[0].path == project.media()[0].path);
    CHECK(loaded.sequence().frameRate == Rational({ 24000, 1001 }));
    CHECK(loaded.sequence().videoTracks[1].clips.size() == 1);
    CHECK(loaded.sequence().videoTracks[1].clips[0].start == 2 * TicksPerSecond);
    CHECK(loaded.toJson(dir).dump() == project.toJson(dir).dump());

    // The media path is stored relative to the project.
    CHECK(project.toJson(dir)["media"][0]["path"].toString() == "media/clip.mov");

    // New ids never collide with loaded ones.
    CHECK(loaded.newId() > loaded.sequence().videoTracks[1].clips[0].id);

    // Undo / redo
    project.removeClips({ project.sequence().videoTracks[1].clips[0].id });
    history.commit(project, "Delete clips");
    CHECK(history.modified());
    CHECK(history.undoLabel() == "Delete clips");

    history.undo(project);
    CHECK(project.sequence().videoTracks[1].clips.size() == 1);
    CHECK(!history.modified());

    history.redo(project);
    CHECK(project.sequence().videoTracks[1].clips.empty());

    std::filesystem::remove_all(dir);
}

void testRippleDelete()
{
    Project project;
    Sequence& sequence = project.sequence();
    const Id voice = project.importMedia("C:/media/voice.wav");
    const Id music = project.importMedia("C:/media/music.flac");
    project.findMedia(voice)->hasAudio = true;

    const Id a1 = sequence.audioTracks[0].id;
    const Id a2 = sequence.audioTracks[1].id;

    project.addClip(voice, a1, 0);
    project.addClip(music, a2, 0);

    // Cut the voice at 2 s and 3 s, ripple delete the middle part.
    project.splitAt(2 * TicksPerSecond, { sequence.audioTracks[0].clips[0].id });
    project.splitAt(3 * TicksPerSecond, { sequence.audioTracks[0].clips[1].id });
    CHECK(sequence.audioTracks[0].clips.size() == 3);

    project.rippleDelete({ sequence.audioTracks[0].clips[1].id });

    const Track& track = sequence.audioTracks[0];
    CHECK(track.clips.size() == 2);
    CHECK(track.clips[1].start == 2 * TicksPerSecond);
    CHECK(track.clips[1].sourceIn == 3 * TicksPerSecond);
    CHECK(track.clips[1].end() == 9 * TicksPerSecond);

    // The music, on another track, did not move.
    CHECK(sequence.audioTracks[1].clips[0].start == 0);
    CHECK(sequence.audioTracks[1].clips[0].duration == 10 * TicksPerSecond);

    // A gap of 1 s before the voice: closed, the music as before.
    for (Clip& clip : sequence.audioTracks[0].clips) {
        clip.start += TicksPerSecond;
    }

    CHECK(!project.closeGap(a1, 0, 2 * TicksPerSecond));    // Not empty
    CHECK(project.closeGap(a1, 0, TicksPerSecond));
    CHECK(sequence.audioTracks[0].clips[0].start == 0);
    CHECK(sequence.audioTracks[0].clips[1].start == 2 * TicksPerSecond);
    CHECK(sequence.audioTracks[1].clips[0].start == 0);
}


void testAnimation()
{
    AnimatedValue value = 2.f;
    CHECK(value.at(123) == 2.f);

    // Not animated: set changes the constant.
    value.set(10, 3.f);
    CHECK(value.at(0) == 3.f && !value.animated());

    // Keyframes: held before and after, linear in between.
    value.addKey(0, Easing::Linear);
    value.set(TicksPerSecond, 5.f);
    CHECK(value.keys.size() == 2);
    CHECK(value.at(-1) == 3.f);
    CHECK(std::abs(value.at(TicksPerSecond / 2) - 4.f) < 1e-4f);
    CHECK(value.at(2 * TicksPerSecond) == 5.f);

    // Ease in-out is symmetric around the middle.
    value.keys[0].easing = Easing::EaseInOut;
    CHECK(std::abs(value.at(TicksPerSecond / 2) - 4.f) < 1e-4f);
    CHECK(value.at(TicksPerSecond / 4) < 3.5f);

    value.removeKey(0);
    value.removeKey(TicksPerSecond);
    CHECK(!value.animated() && value.at(0) == 5.f);
}


void testTextAndFreeze()
{
    Project project;
    Sequence& sequence = project.sequence();

    // A text, with a styled word, saved and loaded back.
    const Id text = project.addTextClip(sequence.videoTracks[2].id, TicksPerSecond, 3 * TicksPerSecond, "Hello ");
    CHECK(text != InvalidId);

    Clip* textClip = project.findClip(text);
    TextSpan bold = { "wörld", TextStyle() };
    bold.style.color = { 1.f, 0.5f, 0.f, 1.f };
    bold.style.outlineWidth = 4.f;
    textClip->text.spans.push_back(bold);
    textClip->text.animation.in = LetterAnimation::Rise;
    textClip->transform.scale.addKey(0);
    textClip->transform.scale.set(TicksPerSecond, 2.f);

    CHECK(textClip->text.letterCount() == 11);

    const Json json = project.toJson({});
    Project loaded;
    loaded.fromJson(Json::parse(json.dump()), {});

    const Clip* loadedText = loaded.findClip(text);
    CHECK(loadedText && loadedText->kind == ClipKind::Text);
    CHECK(loadedText->text.spans.size() == 2);
    CHECK(loadedText->text.spans[1].text == "wörld");
    CHECK(loadedText->text.spans[1].style == bold.style);
    CHECK(loadedText->text.animation.in == LetterAnimation::Rise);
    CHECK(loadedText->transform.scale.keys.size() == 2);
    CHECK(loadedText->transform.scale.at(TicksPerSecond) == 2.f);

    // Freeze frame at 2 s of a 10 s video with its audio.
    const Id media = project.importMedia("C:/media/tutorial.mp4");
    const Id video = project.addClip(media, sequence.videoTracks[0].id, 0);
    const Id still = project.insertFreezeFrame(video, 2 * TicksPerSecond, 3 * TicksPerSecond);

    CHECK(still != InvalidId);

    const Track& v1 = sequence.videoTracks[0];
    const Track& a1 = sequence.soundTracks[0];

    CHECK(v1.clips.size() == 3);
    CHECK(v1.clips[1].frozen && v1.clips[1].sourceIn == 2 * TicksPerSecond);
    CHECK(v1.clips[2].start == 5 * TicksPerSecond && v1.clips[2].sourceIn == 2 * TicksPerSecond);
    CHECK(v1.clips[2].end() == 13 * TicksPerSecond);

    // The audio leaves a silence during the still.
    CHECK(a1.clips.size() == 2);
    CHECK(a1.clips[0].end() == 2 * TicksPerSecond);
    CHECK(a1.clips[1].start == 5 * TicksPerSecond);

    // The text on V3 did not move.
    CHECK(project.findClip(text)->start == TicksPerSecond);
}


void testTransitions()
{
    Project project;
    Sequence& sequence = project.sequence();

    // Two adjacent images on V1, a dissolve of 1 s into the second at 5 s.
    const Id image = project.importMedia("C:/media/still.png");
    const Id a = project.addClip(image, sequence.videoTracks[0].id, 0);
    const Id b = project.addClip(image, sequence.videoTracks[0].id, 5 * TicksPerSecond);
    CHECK(a != InvalidId && b != InvalidId);

    Clip* incoming = project.findClip(b);
    incoming->transitionIn = { TransitionType::Dissolve, TicksPerSecond };

    // Outside the transition: one layer.
    std::vector<LayerRequest> layers = layersAt(sequence, 2 * TicksPerSecond);
    CHECK(layers.size() == 1 && layers[0].clip->id == a && layers[0].opacity == 1.f);

    // Before the cut: the outgoing clip plays, the incoming one holds its
    // first frame on the second slot, fading in.
    layers = layersAt(sequence, 5 * TicksPerSecond - TicksPerSecond / 4);
    CHECK(layers.size() == 2);
    CHECK(layers[0].clip->id == a && layers[0].slot == 0 && !layers[0].still);
    CHECK(layers[1].clip->id == b && layers[1].slot == 1 && layers[1].still && layers[1].localTime == 0);
    CHECK(layers[1].opacity > 0.f && layers[1].opacity < 0.5f);

    // After the cut: the outgoing clip holds its last frame.
    layers = layersAt(sequence, 5 * TicksPerSecond + TicksPerSecond / 4);
    CHECK(layers.size() == 2);
    CHECK(layers[0].clip->id == a && layers[0].slot == 1 && layers[0].still);
    CHECK(layers[0].localTime == project.findClip(a)->duration - 1);
    CHECK(layers[1].clip->id == b && layers[1].slot == 0 && layers[1].opacity > 0.5f);

    // Over: the incoming clip alone.
    layers = layersAt(sequence, 6 * TicksPerSecond);
    CHECK(layers.size() == 1 && layers[0].clip->id == b && layers[0].opacity == 1.f);

    // Push: the pictures slide.
    incoming->transitionIn.type = TransitionType::Push;
    layers = layersAt(sequence, 5 * TicksPerSecond);
    CHECK(layers.size() == 2 && layers[0].offsetX < 0.f && layers[1].offsetX > 0.f);

    // Saved and loaded back.
    Project loaded;
    loaded.fromJson(Json::parse(project.toJson({}).dump()), {});
    CHECK(loaded.findClip(b)->transitionIn.type == TransitionType::Push);
    CHECK(loaded.findClip(b)->transitionIn.duration == TicksPerSecond);
    CHECK(loaded.findClip(a)->transitionIn.type == TransitionType::None);
}


void testTextEditing()
{
    TextContent text;
    text.spans.push_back({ "Hello world", TextStyle() });

    CHECK(textLength(text) == 11);

    // Color "world": the span is cut in two.
    TextStyle before = styleAt(text, 7);
    TextStyle after = before;
    after.color = { 1.f, 0.f, 0.f, 1.f };
    applyStyleChange(text, 6, 11, before, after);

    CHECK(text.spans.size() == 2);
    CHECK(text.spans[0].text == "Hello ");
    CHECK(text.spans[1].text == "world");
    CHECK(text.spans[1].style.color.r == 1.f && text.spans[1].style.color.g == 0.f);

    // Bigger "o w": the size changes, the colors stay.
    before = styleAt(text, 5);
    after = before;
    after.size = 200.f;
    applyStyleChange(text, 4, 7, before, after);

    CHECK(text.spans.size() == 4);
    CHECK(text.spans[1].text == "o " && text.spans[1].style.size == 200.f && text.spans[1].style.color.g == 1.f);
    CHECK(text.spans[2].text == "w" && text.spans[2].style.size == 200.f && text.spans[2].style.color.g == 0.f);

    // Typing takes the style of the character before.
    const size_t end = insertText(text, 11, "!\n\xc3\xa9");
    CHECK(end == 14);
    CHECK(textRange(text, 11, 14) == "!\n\xc3\xa9");
    CHECK(styleAt(text, 14).color.g == 0.f);

    // Erasing merges the spans that become equal.
    eraseText(text, 4, 7);
    CHECK(textRange(text, 0, textLength(text)) == "Hellorld!\n\xc3\xa9");
    CHECK(text.spans.size() == 2);

    // Erasing everything keeps a span, for typing.
    eraseText(text, 0, textLength(text));
    CHECK(text.spans.size() == 1 && text.spans[0].text.empty());
    CHECK(insertText(text, 0, "ok") == 2);

    // Words
    TextContent words;
    words.spans.push_back({ "one two, three", TextStyle() });
    CHECK(wordStart(words, 6) == 4 && wordEnd(words, 6) == 7);
}


// Writes a 16 bits PCM WAV file holding a sine wave.
void writeSineWav(const std::filesystem::path& path, uint32_t sampleRate, uint32_t channels, double seconds, double frequency)
{
    const uint32_t frames = (uint32_t)(seconds * sampleRate);
    const uint32_t dataSize = frames * channels * 2;

    std::ofstream file(path, std::ios::binary);

    auto u32 = [&](uint32_t v) { file.write((const char*)&v, 4); };
    auto u16 = [&](uint16_t v) { file.write((const char*)&v, 2); };

    file.write("RIFF", 4); u32(36 + dataSize); file.write("WAVE", 4);
    file.write("fmt ", 4); u32(16); u16(1); u16((uint16_t)channels); u32(sampleRate);
    u32(sampleRate * channels * 2); u16((uint16_t)(channels * 2)); u16(16);
    file.write("data", 4); u32(dataSize);

    for (uint32_t i = 0; i < frames; i++) {
        const int16_t value = (int16_t)(16000.0 * std::sin(2.0 * 3.14159265358979 * frequency * i / sampleRate));

        for (uint32_t c = 0; c < channels; c++) {
            file.write((const char*)&value, 2);
        }
    }
}


void testDecodeAndMix()
{
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "QuickEditTests";
    std::filesystem::create_directories(dir);
    const std::filesystem::path wav = dir / "sine.wav";

    // Mono 44.1 kHz, decoded to stereo 48 kHz.
    writeSineWav(wav, 44100, 1, 2.0, 440.0);

    const media::ProbeResult probe = media::probe(wav);
    CHECK(probe.hasAudio);
    CHECK(!probe.hasVideo);
    CHECK(probe.sampleRate == 44100);
    CHECK(probe.channels == 1);
    CHECK(std::llabs(probe.duration - 2 * TicksPerSecond) < TicksPerSecond / 100);

    std::atomic<bool> cancel = false;
    auto buffer = std::make_shared<media::AudioBuffer>(media::decodeAudio(wav, 48000, cancel));

    CHECK(buffer->sampleRate == 48000);
    CHECK(std::llabs(buffer->frameCount() - 96000) < 100);

    float peak = 0.f;
    for (float sample : buffer->samples) peak = std::max(peak, std::abs(sample));
    CHECK(peak > 0.3f && peak < 0.6f);

    const media::Waveform waveform = media::Waveform::build(*buffer);
    CHECK(!waveform.levels.empty());
    CHECK(waveform.levels.back().size() == 1);
    CHECK(waveform.levels.back()[0].max > 0.3f);

    // Mix: one clip at 1 s with -6 dB, silent before.
    audio::MixGraph graph;
    graph.sampleRate = 48000;
    graph.tracks.emplace_back().clips.push_back({ 48000, 48000, 0, 0.5f, 0, 0, buffer });

    std::vector<float> output(2 * 48000 * 3);
    audio::mix(graph, 0, 48000 * 3, output.data());

    float before = 0.f, during = 0.f, after = 0.f;
    for (int i = 0; i < 48000; i++) {
        before = std::max(before, std::abs(output[2 * i]));
        during = std::max(during, std::abs(output[2 * (48000 + i)]));
        after = std::max(after, std::abs(output[2 * (96000 + i)]));
    }

    CHECK(before == 0.f);
    CHECK(during > 0.5f * peak - 0.01f && during < 0.5f * peak + 0.01f);
    CHECK(after == 0.f);

    // The edges fade in: the first frame of the clip is almost silent.
    CHECK(std::abs(output[2 * 48000]) < 0.01f);

    // A fade out of half a second: quieter halfway, silent at the end.
    graph.tracks[0].clips[0].fadeOut = 24000;
    audio::mix(graph, 0, 48000 * 3, output.data());

    float early = 0.f, late = 0.f;
    for (int i = 0; i < 4800; i++) {
        early = std::max(early, std::abs(output[2 * (48000 + 4800 + i)]));
        late = std::max(late, std::abs(output[2 * (96000 - 4800 + i)]));
    }

    CHECK(early > 0.5f * peak - 0.01f);
    CHECK(late < 0.2f * peak);

    std::filesystem::remove_all(dir);
}

} // namespace


int main()
{
    testTime();
    testJson();
    testProjectEdits();
    testSaveLoadAndHistory();
    testRippleDelete();
    testAnimation();
    testTextAndFreeze();
    testTransitions();
    testTextEditing();

    try {
        testDecodeAndMix();
    }
    catch (const std::exception& e) {
        std::printf("testDecodeAndMix: %s\n", e.what());
        failures++;
    }

    if (failures == 0) {
        std::printf("All core tests passed\n");
    }

    return failures == 0 ? 0 : 1;
}

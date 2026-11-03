# QuickEdit: architecture

A video editor that is simple to use yet powerful, rendered entirely with
Vulkan. This document describes how the code is organized and the state of
each part.

## Overview

```
           ┌─────────────── UI (ImGui docking, main thread) ─────────────────┐
           │ Media │ Viewer │ Inspector │ Timeline │ Keyframes │ Recorders   │
           └──────────────┬──────────────────────────────────────────────────┘
                          │ edits (snapshots → undo / redo)
                  ┌───────▼────────┐
                  │  core::Project │  pure model, no GPU nor UI, tested
                  └───────┬────────┘
         ┌────────────────┼───────────────────────┐
┌────────▼────────┐ ┌─────▼──────────┐    ┌───────▼──────────┐
│ media::         │ │ render::       │    │ audio::          │
│ VideoReader     │ │ Compositor     │    │ AudioEngine      │
│ FFmpeg + Vulkan │─▶ Vulkan passes  │    │ float mix, VST3, │
│ Video decoding  │ │                │    │ SDL3 output      │
└─────────────────┘ └─────┬──────────┘    └───────┬──────────┘
                          │ VkImage               │ master clock
                  ┌───────▼────────┐              │
                  │ app::Exporter  │◀─────────────┘
                  │ Vulkan Video / │
                  │ NVENC + FFmpeg │
                  └────────────────┘
```

A single `VkDevice` is shared by every module: the UI, the compositor, FFmpeg
(decoding and encoding) and the exporter. Decoded pictures stay on the GPU
from the decoder to the encoder (zero copy) whenever the hardware allows it.

## Modules

### `core/`: the model

- `Time.h`: time is an `int64` in ticks of 1/705,600,000 s. This unit divides
  exactly every usual frame rate (24 to 120 fps) and every audio sample rate
  (44.1 to 192 kHz). NTSC rates go through `Rational`.
- `Project`: media (in folders), sequences (the current one and the others),
  tracks and clips. A sequence has video tracks, each paired with a sound
  track for the sound of its clips, audio tracks (voice, music) and subtitle
  tracks. Clips: media, text, generator (shader) or nested sequence, with a
  link group (the picture and the sound of a file), a transform, a speed, a
  fit mode (fit, fill, native size), crop and frame, fades, a transition in,
  video effects and VST3 effects. Operations: import, placement without
  overlap, split, ripple delete, gaps, freeze frame, clip speed, tracks.
- `Animation`: animated values with keyframes and easing curves.
- `Layers`: the layers to draw at a time (`layersAt`), with transitions and
  nested sequences.
- `Text`, `TextEdit`: rich text (spans with font, size, weight, color,
  outline, shadow, per-letter animations) and its editing.
- `Shaders`, `VideoEffects`, `Subtitles` (SRT import / export).
- `History`: undo / redo by JSON snapshots of the whole project, which share
  their unchanged parts.
- `Json`: minimal JSON reader / writer (exact 64-bit integers).
- `Paths`: the folder of the settings and caches (`%APPDATA%\QuickEdit`,
  `~/.config/quickedit`).

### Project format `.qep`

Readable, versioned JSON (`"version": 1`; newer versions are refused, older
ones are upgraded on load). Media paths are relative to the project when they
are in its folder. Written to a temporary file then renamed, so that a
project is never left truncated. The shaders of the user used in a project
are saved in it.

### `media/`: probing and decoding

`GUIBase` creates the device with a video decode queue when the GPU has one,
and `HwDevice` gives it to FFmpeg (same queues, same locks). `VideoPlayer`
decodes on a thread of its own (`VideoReader`: seeking, frames decoded ahead
while playing). Vulkan Video frames stay `VkImage`s that `VideoFrameRenderer`
converts to RGBA with a compute shader (`shader/yuv_to_rgb.comp`). Other
formats (images, ProRes...) are decoded and converted on the CPU, then
uploaded. `MediaManager` probes the files and decodes their sound in the
background, then builds multi-resolution waveforms; `Thumbnails` builds the
pictures shown along the video clips.

In Debug builds, `quickedit.log` next to the executable receives the SDL,
Vulkan and FFmpeg messages.

### `render/`: compositing

`Compositor` draws the layers from the bottom track to the top one with
dynamic rendering, each one placed by the animated transform of its clip and
its fit mode, cropped and framed (rounded corners, border, shadow) by
`shader/layer.frag`. Video effects are full-screen passes
(`shader/effect_*.frag`) applied before the layer is drawn: the clip's, then
its track's. Nested sequences are rendered into their own target first
(`cmdRenderLevel`). Texts (`FontLibrary`, `TextLayout`) are drawn from a
signed distance field glyph atlas, one instance per glyph, with per-letter
animations. User shaders (generators, transitions, blend modes and
resampling) are compiled at run time with shaderc (`ShaderCompiler`). They
get the sound levels of the frame (`audio::soundLevels()`, from a mix
without effects). A blend shader ends the pass of the level: the layer is
drawn alone, mixed with the level by the shader, and the result drawn over
it. A resampling shader draws the picture at its size in the output before
the layer is drawn. The output can have any size: the layout stays in the
pixels of the sequence.

### `audio/`: playback, mixing, plugins

`buildMixGraph()` flattens the audible tracks into an immutable graph swapped
atomically; `AudioEngine` mixes it in the SDL3 callback and is the clock of
the playback (and plays short pieces while the playhead is dragged). Each
clip goes through its own effects, then its track's; the sound of a nested
sequence goes through the effects of its own clips and tracks first
(`MixTrack::subTracks`). Clips at another speed are resampled.

VST3 plugins (`Vst3Plugin`, SDK 3.8 under the MIT license) process in the
audio thread, their editors in windows of their own. `PluginScanner` lists
every audio effect of every installed `.vst3` (a shell such as WaveShell
holds many) in the background, with a cache. `EffectRack` keeps the plugin
instances in line with the project. `Recorder` records voice-overs.
`Loudness` measures the integrated loudness (ITU-R BS.1770, EBU R128) for the
loudness normalization at export.

### `capture/`: screen and camera recording

`ScreenRecorder` records a screen or a window (the `gfxcapture` filter of
FFmpeg, Windows Graphics Capture, the pictures staying on the GPU up to
NVENC / AMF) or a camera (DirectShow, its formats read with
`listCameraFormats`), with the sound of the computer (WASAPI loopback).
Windows only for now.

### `app/`: the application

- `GUIBase`: SDL3 window without borders, Vulkan
  device and swapchain, ImGui with docking and multiple viewports.
- `GUI`: the panels, menus, toolbars, shortcuts, dialogs and the playback.
- `TimelineView`: ruler, track headers, clips; selection (also with a
  rectangle), moves, trims, snapping, tools (select, blade, slip, select
  forward), context menus of clips and tracks.
- `Inspector`, `KeyframeEditor`, `ViewerTextEditor` (text editing in the
  viewer), `SubtitleGenerator` (whisper.cpp), `EffectRack`, `Exporter`.
- `RecordingScene`, `SceneRecorder.cpp`: scenes of several sources recorded
  at once (one `ScreenRecorder` per source, the microphone added to the first
  file), each placed on a video track of its own in its rectangle; scenes
  saved in `scenes.json` of the settings folder.
- `Widgets.h`: small widgets shared by the panels.
- `I18n`: translations of the interface. `tr()` looks the English text up in
  the catalog of the language (`lang/<code>.json`), keeping the icons at the
  start of a label and its ImGui ID (`##`, `###`). The windows looked up by
  name have a stable ID (`tr("Timeline###Timeline")`). The labels of the undo
  history are translated when shown. `tools/update_translations.py` keeps
  the catalogs in line with the code.

### Export

`Exporter` (a thread of its own) composites each frame as the viewer does,
with its own readers and compositor, on the second queue of the graphics
family. `Nv12Converter` writes the picture as NV12 (BT.709) straight into a
Vulkan frame of the encoder (`h264_vulkan`, `hevc_vulkan`); without Vulkan
encoding, the picture is read back and converted for NVENC, AMF, Quick Sync
or Media Foundation. `av1_vulkan` (FFmpeg 8.1) gives a corrupt stream on
NVIDIA: it comes after the dedicated AV1 encoders. Sound: the mix of the
playback, optionally normalized to a loudness target, in AAC.

## Threads

| Thread           | Role                                                    |
|------------------|---------------------------------------------------------|
| Main             | UI, edits of the model, graphics submissions            |
| Decoding (n)     | reading ahead, per active video                          |
| Real-time audio  | mixing and plugins, no allocation nor lock               |
| Background tasks | probing, thumbnails, waveforms, plugin scan, export      |

The model is only modified by the main thread; the other threads work on
immutable copies published after each edit.

## Command line (tests)

```
QuickEdit project.qep [--time seconds] [--select clipId] [--edit]
                      [--effect-editor slotId] [--edit-shader name]
                      [--export video.mp4 [--codec h264|h265|av1] [--height h] [--fps n] [--lufs target]]
                      [--drop file x y] [--record seconds]
                      [--record-screen seconds [--source index]]
                      [--transcribe model] [--quit-after seconds]
```

## Roadmap

Done: editing model and timeline, Vulkan Video decoding, compositing, texts,
keyframes and curves, transitions, VST3, export, voice-over / screen / camera
recording, video effects, user shaders, nested sequences, subtitles with
automatic transcription, clip speed, timeline tools, loudness normalization,
interface in English and French.

Next: Linux check of the capture and the plugin editors,
spectrum display of the sound.

## Building

See the [README](../README.md).

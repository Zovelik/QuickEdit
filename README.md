# QuickEdit

A video editor that is simple to use yet powerful, rendered entirely with
Vulkan. Made for videos built around a voice-over: record and clean the voice
quickly, put music under it, and screen captures, videos and animated texts
over it.

Windows and Linux. C++20, SDL3, Dear ImGui, Vulkan, FFmpeg.

## Features

- **Timeline**: video tracks with their sound, audio tracks, subtitle tracks;
  snapping, rectangle selection, ripple delete, gaps, linked picture and
  sound. Tools: select, blade, slip, select forward. Clip speed, freeze
  frame. Several sequences, nested in one another.
- **GPU everywhere**: decoding with Vulkan Video, compositing, effects and
  encoding (Vulkan Video, NVENC, AMF) without the pictures leaving the GPU.
- **Pictures**: position, scale, rotation and opacity with keyframes and
  curves; fit, fill or native size; crop, rounded corners, border, shadow;
  transitions; video effects (color, blur, sharpen, vignette, chroma key,
  pixelate) per clip or per track; generators and transitions written as
  GLSL shaders, with a built-in editor.
- **Texts**: rich text edited in the viewer, per-letter animations.
- **Sound**: waveforms, fades, VST3 effects per clip and per track (shells
  such as WaveShell included), scrubbing, loudness normalization at export.
- **Recording**: voice-over, screen, window or camera (format and frame rate
  of the camera), with the sound of the computer.
- **Subtitles**: SRT import and export, burnt in at export, or generated from
  the speech with whisper.cpp on the GPU.
- **Export**: H.264, H.265 and AV1 to MP4, MOV or MKV, at any size and frame
  rate.

## Shortcuts

| Keys                 | Action                                  |
|----------------------|-----------------------------------------|
| Space                | Play / pause                            |
| Left / Right         | Previous / next frame                   |
| Home / End           | Start / end of the sequence             |
| Ctrl+K               | Split at the playhead                   |
| Delete / Shift+Del   | Delete / ripple delete                  |
| V, C, Y, A           | Select, blade, slip, select forward     |
| N                    | Snapping                                |
| T / Shift+T          | Add a text / a subtitle                 |
| Shift+F              | Freeze frame                            |
| Ctrl+D               | Add or remove a transition              |
| R / Shift+R          | Record a voice-over / the screen        |
| Ctrl+Z / Ctrl+Y      | Undo / redo                             |
| Ctrl+Wheel, Ctrl+0   | Zoom the timeline, show the whole of it |

## Building

### Requirements

- CMake 3.15 or newer and Ninja.
- A C++20 compiler: Visual Studio 2022 or newer on Windows, GCC 12+ or Clang
  15+ on Linux.
- The Vulkan SDK 1.4 (with `glslc` and `shaderc`).
- A GPU with Vulkan 1.3; Vulkan Video decoding and encoding are used when
  available.

### Dependencies

Git submodules: SDL3, Dear ImGui, ImGuiColorTextEdit.

```
git submodule update --init --recursive
```

Downloaded, ignored by git:

- **FFmpeg 8.1** (LGPL). Windows: the shared build
  `ffmpeg-n8.1-latest-win64-lgpl-shared-8.1.zip` from
  https://github.com/BtbN/FFmpeg-Builds, extracted into
  `3rdparty/ffmpeg/win64`. Linux: the FFmpeg development packages
  (`libavcodec-dev libavformat-dev libavfilter-dev libavdevice-dev
  libswscale-dev libswresample-dev`).
- **VST3 SDK 3.8.1** (MIT):

  ```
  git clone --depth 1 --branch v3.8.1_build_84 https://github.com/steinbergmedia/vst3sdk.git 3rdparty/vst3sdk
  cd 3rdparty/vst3sdk
  git submodule update --init --depth 1 base pluginterfaces public.sdk
  ```

- **whisper.cpp 1.8.7** (MIT), optional, for the automatic subtitles:

  ```
  git clone --depth 1 --branch v1.8.7 https://github.com/ggml-org/whisper.cpp.git 3rdparty/whisper.cpp
  ```

  The speech models are downloaded from the application, into
  `Documents/QuickEdit/Models`.

### Build and test

```
cmake -G Ninja -S . -B out/build/x64-Release -DCMAKE_BUILD_TYPE=Release
cmake --build out/build/x64-Release
ctest --test-dir out/build/x64-Release
```

On Windows, run these from a Visual Studio developer prompt, or open the
folder in Visual Studio (`CMakeSettings.json`).

## Languages

The interface is in English and French (View > Language; the language of
the system by default). The translations are in `lang/<code>.json`, keyed by
the English texts. After a change of the texts of the code, run
`python tools/update_translations.py`: it adds the new texts to every
catalog (empty: shown in English until translated) and removes the old ones.
A new language is a new catalog, with its name in `"_language"`.

## Where things go

- Settings (language) and caches: `%APPDATA%\QuickEdit` on Windows,
  `~/.config/quickedit` on Linux.
- Recordings: a `Recordings` folder next to the project, or
  `Documents/QuickEdit/Recordings` for a project not saved yet.
- Shaders of the user: `Documents/QuickEdit/Shaders`.

## Documentation

[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) describes the modules, the
threads and the formats.

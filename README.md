# QuickEdit

A video editor that is simple to use yet powerful, rendered entirely with
Vulkan. Made for videos built around a voice-over: record and clean the voice
quickly, put music under it, and screen captures, videos and animated texts
over it.

Windows and Linux. C++20, SDL3, Dear ImGui, Vulkan, FFmpeg.

## Features

- **Timeline**: video tracks with their sound, audio tracks, subtitle tracks;
  snapping, rectangle selection, ripple delete, gaps, linked picture and
  sound; clips moved or dropped over others overwrite what they cover. The
  audio streams of a file unfold under its track, each one muted or
  removed. Tools: select, blade, slip, select forward. Clip speed, freeze
  frame. Several sequences, nested in one another.
- **GPU everywhere**: decoding with Vulkan Video, compositing, effects and
  encoding (Vulkan Video, NVENC, AMF) without the pictures leaving the GPU.
- **Pictures**: position, scale, rotation and opacity with keyframes and
  curves; fit, fill or native size; bilinear, bicubic, Lanczos or nearest
  sampling; crop, rounded corners, border, shadow;
  transitions; video effects (color, blur, sharpen, vignette, chroma key,
  pixelate) per clip or per track; generators, transitions, blend modes and
  resampling written as GLSL shaders (that can follow the sound), with a
  built-in editor.
- **Texts**: rich text edited in the viewer, per-letter animations.
- **Sound**: waveforms, fades, VST3 effects per clip and per track (shells
  such as WaveShell included), scrubbing, loudness normalization at export.
- **Recording**: voice-over, screen, window or camera (format and frame rate
  of the camera), with the sound of the computer. On Linux: X11 screens and
  windows, V4L2 cameras; under Wayland the screen or window chosen in the
  dialog of the system (desktop portal and PipeWire), remembered.
- **Scenes**: several screens, windows and cameras recorded at once, placed
  in the frame beforehand as in OBS; each one gets a file and a track of its
  own, and stays movable. Scenes are saved for later.
- **Subtitles**: SRT import and export, burnt in at export, or generated from
  the speech with whisper.cpp on the GPU.
- **Export**: H.264, H.265 and AV1 to MP4, MOV or MKV, at any size and frame
  rate.
- **AI control**: an MCP server (Model Context Protocol) lets an AI assistant
  such as Claude edit the project you have open: see below.

## AI control (MCP)

Edit > Preferences > AI Control starts a Model Context Protocol server on
`http://127.0.0.1:8765/mcp` (this computer only; off by default). An AI
assistant connected to it sees the project (tracks, clips, media, rendered
frames, transcriptions) and edits it live: media, clips, texts, titles,
transitions, effects, keyframes, subtitles, shaders it writes and compiles,
export. Each of its edits is a step of the history: undo it as any other.

With Claude Code:

```
claude mcp add --transport http quickedit http://127.0.0.1:8765/mcp
```

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
| Ctrl+Shift+R         | Record a scene (several sources)        |
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

Git submodules: SDL3, Dear ImGui, ImGuiColorTextEdit, cpp-httplib.

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

### Packages

```
cpack -C Release --config out/build/x64-Release/CPackConfig.cmake
```

- Windows: an installer (NSIS, which must be installed) and a zip. The
  installer puts QuickEdit in `Program Files\QuickEdit` with a Start menu
  shortcut and an optional desktop shortcut, opens the `.qep` projects with
  it, and registers an uninstaller. The uninstaller offers to delete the
  settings and caches of the user who runs it (kept by default, and when it
  runs silently, as before an upgrade); the user's files in
  `Documents\QuickEdit` always stay.
- Linux: an AppImage (a recent CMake, 4.3 in the workflow, with
  `appimagetool` and `patchelf` on the `PATH`), with a desktop entry, the
  `.qep` file type and AppStream data. FFmpeg and shaderc are inside it; the
  Vulkan loader is the one of the system.

The Linux build of the workflow runs locally in Docker (Ubuntu 22.04, the
same compiler, SDKs and AppImage tools), from any system:

```
docker build -t quickedit-linux tools/linux
docker run --rm -e PACKAGE=1 -v "$PWD:/src:ro" -v quickedit-build:/work quickedit-linux bash /src/tools/linux/build.sh
```

The GitHub Actions workflow (`.github/workflows/build.yml`) runs for a
published release: it builds, tests and packages QuickEdit, and attaches the
packages to the release (a manual run keeps them as artifacts of the run). The
version is the last git tag (`v1.2.0` gives 1.2.0).

## Languages

The interface is in English and French (View > Language; the language of
the system by default). The translations are in `lang/<code>.json`, keyed by
the English texts. After a change of the texts of the code, run
`python tools/update_translations.py`: it adds the new texts to every
catalog (empty: shown in English until translated) and removes the old ones.
A new language is a new catalog, with its name in `"_language"`.

## Where things go

- Settings (language, window layout) and caches: `%APPDATA%\QuickEdit` on Windows,
  `~/.config/quickedit` on Linux.
- Recordings: a `Recordings` folder next to the project, or
  `Documents/QuickEdit/Recordings` for a project not saved yet.
- Shaders of the user: `Documents/QuickEdit/Shaders`.
- Crash dumps (Windows): a `crashes` folder in the settings folder. They are
  read with the symbols of the same build (`QuickEdit.pdb`, kept as the
  `QuickEdit-symbols` artifact of the GitHub Actions run).

## Documentation

[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) describes the modules, the
threads and the formats.

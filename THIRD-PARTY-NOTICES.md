# Third-party notices

QuickEdit is distributed under the BSD 3-Clause License (see `LICENSE`). It
uses the following libraries and resources, each under its own license. The
packages install the license texts in `licenses/` (Windows) or
`share/quickedit/licenses/` (Linux).

| Component | Use | License | Linking |
|---|---|---|---|
| [FFmpeg](https://ffmpeg.org) 8.1 | decoding, encoding, filters, capture | LGPL-2.1-or-later | shared libraries (DLL / .so), replaceable |
| [SDL3](https://libsdl.org) | window, input, audio devices | Zlib | static |
| [Dear ImGui](https://github.com/ocornut/imgui) | user interface | MIT | static |
| [ImGuiColorTextEdit](https://github.com/goossens/ImGuiColorTextEdit) | shader editor | MIT | static |
| [cpp-httplib](https://github.com/yhirose/cpp-httplib) | MCP server | MIT | header only |
| [VST3 SDK](https://github.com/steinbergmedia/vst3sdk) 3.8 | VST3 plugin hosting | MIT | static |
| [whisper.cpp / ggml](https://github.com/ggml-org/whisper.cpp) | automatic subtitles (optional) | MIT | static |
| [shaderc](https://github.com/google/shaderc) (with glslang, SPIRV-Tools) | user shaders compilation | Apache-2.0 (glslang: BSD-3-Clause and others) | shared library |
| [Inter](https://rsms.me/inter/) | UI font (embedded) | SIL Open Font License 1.1 | embedded data |
| [Material Icons](https://github.com/google/material-design-icons) | UI icons (embedded) | Apache-2.0 | embedded data |
| Vulkan loader | GPU access | Apache-2.0 | system library, not distributed |

## FFmpeg

The FFmpeg libraries are the LGPL shared builds of
[BtbN/FFmpeg-Builds](https://github.com/BtbN/FFmpeg-Builds)
(`ffmpeg-n8.1-latest-*-lgpl-shared-8.1`), configured without `--enable-gpl`
or `--enable-nonfree`. They are not modified. Their source code is at
https://github.com/FFmpeg/FFmpeg (tag `n8.1`) and the build scripts at
https://github.com/BtbN/FFmpeg-Builds. The libraries can be replaced by other
builds of the same version.

This software uses libraries from the FFmpeg project under the LGPLv2.1.

## Patents

Some formats (H.264, HEVC, AAC...) may be covered by patents in some
countries. QuickEdit uses the encoders and decoders of FFmpeg and of the GPU
drivers; no patent license is granted by this distribution.

## Trademarks

VST is a registered trademark of Steinberg Media Technologies GmbH. Vulkan is
a registered trademark of the Khronos Group Inc. Other names are trademarks
of their respective owners.

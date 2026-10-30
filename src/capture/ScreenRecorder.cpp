#include <capture/ScreenRecorder.h>

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <audioclient.h>
#include <dwmapi.h>
#include <mmdeviceapi.h>
#include <mmreg.h>
#include <dshow.h>
#include <dvdmedia.h>

#endif

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavdevice/avdevice.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavformat/avformat.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/channel_layout.h>
#include <libavutil/hwcontext.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

#include <SDL3/SDL.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <tuple>


namespace capture
{

namespace {

constexpr int AudioSampleRate = 48000;


// Opens a camera and its decoder: in the format of the source when one is
// chosen, else (best) the largest usual size first, in MJPEG (what USB
// cameras give at that size), then what the camera gives by default.
bool openCamera(const CaptureSource& source, bool best, AVFormatContext*& input, AVCodecContext*& decoder, int& streamIndex, std::string& error)
{
    avdevice_register_all();

    const AVInputFormat* format = av_find_input_format("dshow");

    if (!format) {
        error = "No camera support in this build of FFmpeg.";
        return false;
    }

    // Formats with their frame rate (0: the camera's choice).
    std::vector<std::pair<CameraFormat, double>> attempts;

    if (!source.format.automatic()) {
        attempts.emplace_back(source.format, source.frameRate);
    }
    else if (best) {
        // The largest MJPEG up to 1080p at its highest rate: left alone, a
        // camera may pick its slowest one.
        for (const CameraFormat& format : listCameraFormats(source)) {
            if (format.codec == "mjpeg" && format.height <= 1080) {
                attempts.emplace_back(format, format.frameRates.empty() ? 0.0 : format.frameRates.front());
                break;
            }
        }

        attempts.emplace_back(CameraFormat{ 1920, 1080, "mjpeg" }, 0.0);
        attempts.emplace_back(CameraFormat{ 1280, 720, "mjpeg" }, 0.0);
    }

    attempts.emplace_back(CameraFormat(), 0.0);

    const std::string url = "video=" + source.device;
    int result = -1;

    for (const auto& [attempt, frameRate] : attempts) {
        AVDictionary* options = nullptr;
        av_dict_set(&options, "rtbufsize", "256M", 0);

        if (!attempt.automatic()) {
            av_dict_set(&options, "video_size", (std::to_string(attempt.width) + "x" + std::to_string(attempt.height)).c_str(), 0);

            if (!attempt.codec.empty()) {
                av_dict_set(&options, "vcodec", attempt.codec.c_str(), 0);
            }
            else if (!attempt.pixelFormat.empty()) {
                av_dict_set(&options, "pixel_format", attempt.pixelFormat.c_str(), 0);
            }

            if (frameRate > 0.0) {
                char rate[32];
                std::snprintf(rate, sizeof(rate), "%.3f", frameRate);
                av_dict_set(&options, "framerate", rate, 0);
            }
        }

        result = avformat_open_input(&input, url.c_str(), format, &options);
        av_dict_free(&options);

        if (result >= 0) {
            break;
        }

        SDL_Log("Camera %s: format %s refused", source.name.c_str(), attempt.label().c_str());
    }

    if (result < 0) {
        error = "Could not open the camera (used by another application?).";
        return false;
    }

    avformat_find_stream_info(input, nullptr);
    streamIndex = av_find_best_stream(input, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);

    if (streamIndex < 0) {
        error = "The camera gives no pictures.";
        avformat_close_input(&input);
        return false;
    }

    const AVCodecParameters* parameters = input->streams[streamIndex]->codecpar;
    const AVCodec* codec = avcodec_find_decoder(parameters->codec_id);
    decoder = avcodec_alloc_context3(codec);

    if (!codec || avcodec_parameters_to_context(decoder, parameters) < 0 || avcodec_open2(decoder, codec, nullptr) < 0) {
        error = "Could not decode the pictures of the camera.";
        avcodec_free_context(&decoder);
        avformat_close_input(&input);
        return false;
    }

    return true;
}


// Scales a picture into the preview, letterboxed.
void toPreview(SwsContext*& sws, const AVFrame* frame, std::vector<uint8_t>& rgba, int width, int height)
{
    const float fit = std::min((float)width / (float)frame->width, (float)height / (float)frame->height);
    const int w = std::max(2, (int)((float)frame->width * fit) & ~1);
    const int h = std::max(2, (int)((float)frame->height * fit) & ~1);
    const int x = (width - w) / 2;
    const int y = (height - h) / 2;

    sws = sws_getCachedContext(sws, frame->width, frame->height, (AVPixelFormat)frame->format, w, h, AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr, nullptr, nullptr);

    if (!sws) {
        return;
    }

    std::fill(rgba.begin(), rgba.end(), (uint8_t)0);

    for (size_t i = 3; i < rgba.size(); i += 4) {
        rgba[i] = 255;
    }

    uint8_t* data[4] = { rgba.data() + ((size_t)y * width + x) * 4, nullptr, nullptr, nullptr };
    const int linesize[4] = { width * 4, 0, 0, 0 };

    sws_scale(sws, frame->data, frame->linesize, 0, frame->height, data, linesize);
}


// Description of the capture filter of a screen or a window.
std::string captureFilter(const CaptureSource& source, int maxFrameRate, bool cursor)
{
    const std::string option = (source.kind == CaptureSource::Kind::Monitor)
        ? "hmonitor=" + std::to_string(source.handle)
        : "hwnd=" + std::to_string(source.handle);

    return "gfxcapture=" + option + ":max_framerate=" + std::to_string(maxFrameRate) + ":capture_cursor=" + (cursor ? "1" : "0");
}


// A filter graph from a description, ending in a buffer sink.
AVFilterGraph* openGraph(const std::string& description, AVFilterContext*& sink, int& result)
{
    AVFilterGraph* graph = avfilter_graph_alloc();
    AVFilterInOut* inputs = nullptr;
    AVFilterInOut* outputs = nullptr;

    result = avfilter_graph_create_filter(&sink, avfilter_get_by_name("buffersink"), "out", nullptr, nullptr, graph);

    if (result >= 0) {
        result = avfilter_graph_parse2(graph, description.c_str(), &inputs, &outputs);
    }

    if (result >= 0 && outputs) {
        result = avfilter_link(outputs->filter_ctx, outputs->pad_idx, sink, 0);
    }

    avfilter_inout_free(&inputs);
    avfilter_inout_free(&outputs);

    if (result >= 0) {
        result = avfilter_graph_config(graph, nullptr);
    }

    if (result < 0) {
        avfilter_graph_free(&graph);
    }

    return graph;
}


std::string averror(int code)
{
    char text[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(code, text, sizeof(text));
    return text;
}


#ifdef _WIN32

std::string utf8(const std::wstring& text)
{
    if (text.empty()) {
        return {};
    }

    const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), (int)text.size(), nullptr, 0, nullptr, nullptr);
    std::string result((size_t)size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), (int)text.size(), result.data(), size, nullptr, nullptr);

    return result;
}


BOOL CALLBACK addMonitor(HMONITOR monitor, HDC, LPRECT, LPARAM data)
{
    auto* sources = reinterpret_cast<std::vector<CaptureSource>*>(data);

    MONITORINFOEXW info = {};
    info.cbSize = sizeof(info);
    GetMonitorInfoW(monitor, &info);

    const int count = 1 + (int)std::count_if(sources->begin(), sources->end(),
        [](const CaptureSource& source) { return source.kind == CaptureSource::Kind::Monitor; });

    CaptureSource source;
    source.kind = CaptureSource::Kind::Monitor;
    source.handle = (uint64_t)(uintptr_t)monitor;
    source.name = "Screen " + std::to_string(count) + "  (" +
        std::to_string(info.rcMonitor.right - info.rcMonitor.left) + " x " +
        std::to_string(info.rcMonitor.bottom - info.rcMonitor.top) +
        ((info.dwFlags & MONITORINFOF_PRIMARY) ? ", main)" : ")");

    sources->push_back(std::move(source));

    return TRUE;
}


BOOL CALLBACK addWindow(HWND window, LPARAM data)
{
    auto* sources = reinterpret_cast<std::vector<CaptureSource>*>(data);

    // The windows of the task bar: visible, titled, not owned, not tool
    // windows, not hidden by the system (other virtual desktops, UWP).
    if (!IsWindowVisible(window) || GetWindow(window, GW_OWNER) || (GetWindowLongW(window, GWL_EXSTYLE) & WS_EX_TOOLWINDOW)) {
        return TRUE;
    }

    BOOL cloaked = FALSE;
    DwmGetWindowAttribute(window, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));

    const int length = GetWindowTextLengthW(window);

    if (cloaked || length == 0) {
        return TRUE;
    }

    // Not the windows of QuickEdit.
    DWORD process = 0;
    GetWindowThreadProcessId(window, &process);

    if (process == GetCurrentProcessId()) {
        return TRUE;
    }

    std::wstring title((size_t)length + 1, L'\0');
    title.resize((size_t)GetWindowTextW(window, title.data(), length + 1));

    // The application, after the title.
    std::wstring exe;

    if (HANDLE handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process)) {
        wchar_t path[MAX_PATH] = {};
        DWORD size = MAX_PATH;

        if (QueryFullProcessImageNameW(handle, 0, path, &size)) {
            exe = std::filesystem::path(path).stem().wstring();
        }

        CloseHandle(handle);
    }

    CaptureSource source;
    source.kind = CaptureSource::Kind::Window;
    source.handle = (uint64_t)(uintptr_t)window;
    source.name = utf8(title) + (exe.empty() ? "" : "  (" + utf8(exe) + ")");

    sources->push_back(std::move(source));

    return TRUE;
}

#endif

} // namespace


std::vector<CaptureSource> listSources()
{
    std::vector<CaptureSource> sources;

#ifdef _WIN32
    EnumDisplayMonitors(nullptr, nullptr, addMonitor, (LPARAM)&sources);

    // Cameras
    avdevice_register_all();
    AVDeviceInfoList* devices = nullptr;

    if (const AVInputFormat* format = av_find_input_format("dshow");
        format && avdevice_list_input_sources(format, nullptr, nullptr, &devices) >= 0) {
        for (int i = 0; i < devices->nb_devices; i++) {
            const AVDeviceInfo* device = devices->devices[i];
            const bool video = std::find(device->media_types, device->media_types + device->nb_media_types, AVMEDIA_TYPE_VIDEO)
                != device->media_types + device->nb_media_types;

            if (video && device->device_name) {
                CaptureSource source;
                source.kind = CaptureSource::Kind::Camera;
                source.handle = (uint64_t)sources.size() + 1;
                source.device = device->device_name;
                source.name = device->device_description ? device->device_description : device->device_name;
                sources.push_back(std::move(source));
            }
        }
    }

    avdevice_free_list_devices(&devices);

    EnumWindows(addWindow, (LPARAM)&sources);
#endif

    return sources;
}


std::string CameraFormat::label() const
{
    if (automatic()) {
        return "Automatic";
    }

    std::string encoding = !codec.empty() ? codec : pixelFormat;

    if (encoding == "mjpeg") encoding = "MJPEG";
    else if (encoding == "h264") encoding = "H.264";
    else if (encoding == "yuyv422") encoding = "YUY2";
    else if (encoding == "nv12") encoding = "NV12";
    else if (encoding == "yuv420p") encoding = "I420";
    else if (encoding == "uyvy422") encoding = "UYVY";
    else if (encoding == "bgr24" || encoding == "bgr0") encoding = "RGB";

    return std::to_string(width) + " x " + std::to_string(height) + "  " + encoding;
}


#ifdef _WIN32

namespace {

std::string toUtf8(const wchar_t* text)
{
    const int size = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    std::string result(size > 0 ? (size_t)size - 1 : 0, '\0');

    if (size > 1) {
        WideCharToMultiByte(CP_UTF8, 0, text, -1, result.data(), size, nullptr, nullptr);
    }

    return result;
}


// The FFmpeg names of the encoding of a DirectShow media subtype: decoder,
// or pixel format of raw pictures. False when not handled.
bool ffmpegEncoding(const GUID& subtype, std::string& codec, std::string& pixelFormat)
{
    if (IsEqualGUID(subtype, MEDIASUBTYPE_RGB24)) { pixelFormat = "bgr24"; return true; }
    if (IsEqualGUID(subtype, MEDIASUBTYPE_RGB32)) { pixelFormat = "bgr0"; return true; }

    // The others are FOURCC subtypes.
    switch (subtype.Data1) {
        case MAKEFOURCC('M', 'J', 'P', 'G'): codec = "mjpeg"; return true;
        case MAKEFOURCC('H', '2', '6', '4'): codec = "h264"; return true;
        case MAKEFOURCC('Y', 'U', 'Y', '2'): pixelFormat = "yuyv422"; return true;
        case MAKEFOURCC('N', 'V', '1', '2'): pixelFormat = "nv12"; return true;
        case MAKEFOURCC('I', '4', '2', '0'):
        case MAKEFOURCC('I', 'Y', 'U', 'V'): pixelFormat = "yuv420p"; return true;
        case MAKEFOURCC('U', 'Y', 'V', 'Y'): pixelFormat = "uyvy422"; return true;
        default: return false;
    }
}


void freeMediaType(AM_MEDIA_TYPE* type)
{
    if (!type) {
        return;
    }

    if (type->cbFormat) CoTaskMemFree(type->pbFormat);
    if (type->pUnk) type->pUnk->Release();
    CoTaskMemFree(type);
}


// The capture filter of a camera: its DirectShow moniker whose name FFmpeg
// shows (':' as '_'), or its friendly name.
IBaseFilter* findCameraFilter(const CaptureSource& camera)
{
    ICreateDevEnum* devices = nullptr;
    IEnumMoniker* monikers = nullptr;
    IBaseFilter* filter = nullptr;

    if (FAILED(CoCreateInstance(CLSID_SystemDeviceEnum, nullptr, CLSCTX_INPROC_SERVER, IID_ICreateDevEnum, (void**)&devices))) {
        return nullptr;
    }

    if (devices->CreateClassEnumerator(CLSID_VideoInputDeviceCategory, &monikers, 0) == S_OK) {
        IMoniker* moniker = nullptr;

        while (!filter && monikers->Next(1, &moniker, nullptr) == S_OK) {
            std::string uniqueName;
            std::string friendlyName;
            LPOLESTR displayName = nullptr;

            if (SUCCEEDED(moniker->GetDisplayName(nullptr, nullptr, &displayName)) && displayName) {
                uniqueName = toUtf8(displayName);
                std::replace(uniqueName.begin(), uniqueName.end(), ':', '_');
                CoTaskMemFree(displayName);
            }

            IPropertyBag* bag = nullptr;

            if (SUCCEEDED(moniker->BindToStorage(nullptr, nullptr, IID_IPropertyBag, (void**)&bag))) {
                VARIANT name;
                VariantInit(&name);

                if (SUCCEEDED(bag->Read(L"FriendlyName", &name, nullptr)) && name.vt == VT_BSTR) {
                    friendlyName = toUtf8(name.bstrVal);
                }

                VariantClear(&name);
                bag->Release();
            }

            if (uniqueName == camera.device || friendlyName == camera.device) {
                moniker->BindToObject(nullptr, nullptr, IID_IBaseFilter, (void**)&filter);
            }

            moniker->Release();
        }

        monikers->Release();
    }

    devices->Release();
    return filter;
}

} // namespace

#endif


std::vector<CameraFormat> listCameraFormats(const CaptureSource& camera)
{
    std::vector<CameraFormat> formats;

#ifdef _WIN32
    const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    IBaseFilter* filter = findCameraFilter(camera);
    ICaptureGraphBuilder2* builder = nullptr;
    IAMStreamConfig* config = nullptr;

    if (filter && SUCCEEDED(CoCreateInstance(CLSID_CaptureGraphBuilder2, nullptr, CLSCTX_INPROC_SERVER, IID_ICaptureGraphBuilder2, (void**)&builder)) &&
        SUCCEEDED(builder->FindInterface(&PIN_CATEGORY_CAPTURE, &MEDIATYPE_Video, filter, IID_IAMStreamConfig, (void**)&config))) {
        int count = 0;
        int size = 0;

        if (SUCCEEDED(config->GetNumberOfCapabilities(&count, &size)) && size == sizeof(VIDEO_STREAM_CONFIG_CAPS)) {
            for (int i = 0; i < count; i++) {
                VIDEO_STREAM_CONFIG_CAPS caps = {};
                AM_MEDIA_TYPE* type = nullptr;

                if (FAILED(config->GetStreamCaps(i, &type, (BYTE*)&caps)) || !type) {
                    continue;
                }

                CameraFormat format;
                REFERENCE_TIME frameTime = 0;

                if (IsEqualGUID(type->formattype, FORMAT_VideoInfo) && type->cbFormat >= sizeof(VIDEOINFOHEADER)) {
                    const VIDEOINFOHEADER* header = (const VIDEOINFOHEADER*)type->pbFormat;
                    format.width = header->bmiHeader.biWidth;
                    format.height = std::abs(header->bmiHeader.biHeight);
                    frameTime = header->AvgTimePerFrame;
                }
                else if (IsEqualGUID(type->formattype, FORMAT_VideoInfo2) && type->cbFormat >= sizeof(VIDEOINFOHEADER2)) {
                    const VIDEOINFOHEADER2* header = (const VIDEOINFOHEADER2*)type->pbFormat;
                    format.width = header->bmiHeader.biWidth;
                    format.height = std::abs(header->bmiHeader.biHeight);
                    frameTime = header->AvgTimePerFrame;
                }

                const bool known = format.width > 0 && format.height > 0 && ffmpegEncoding(type->subtype, format.codec, format.pixelFormat);
                freeMediaType(type);

                if (!known) {
                    continue;
                }

                // The rates of this entry: its default, and its range.
                std::vector<double> rates;

                for (REFERENCE_TIME interval : { frameTime, caps.MinFrameInterval, caps.MaxFrameInterval }) {
                    if (interval > 0) {
                        rates.push_back(std::round(1e7 / (double)interval * 100.0) / 100.0);
                    }
                }

                auto same = std::find_if(formats.begin(), formats.end(), [&](const CameraFormat& other) { return other.sameMode(format); });

                if (same == formats.end()) {
                    formats.push_back(format);
                    same = formats.end() - 1;
                }

                for (double rate : rates) {
                    if (std::find(same->frameRates.begin(), same->frameRates.end(), rate) == same->frameRates.end()) {
                        same->frameRates.push_back(rate);
                    }
                }
            }
        }
    }

    if (config) config->Release();
    if (builder) builder->Release();
    if (filter) filter->Release();

    if (SUCCEEDED(init)) {
        CoUninitialize();
    }

    // The largest first; compressed before raw for a same size.
    std::sort(formats.begin(), formats.end(), [](const CameraFormat& a, const CameraFormat& b) {
        return std::make_tuple(a.width * a.height, a.width, !a.codec.empty(), a.codec, a.pixelFormat) >
               std::make_tuple(b.width * b.height, b.width, !b.codec.empty(), b.codec, b.pixelFormat);
    });

    for (CameraFormat& format : formats) {
        std::sort(format.frameRates.begin(), format.frameRates.end(), std::greater<double>());
    }

    SDL_Log("Camera %s: %d formats", camera.name.c_str(), (int)formats.size());
#else
    (void)camera;
#endif

    return formats;
}


bool addAudioStream(const std::filesystem::path& recording, const std::filesystem::path& audio,
    const std::string& title, std::string& error)
{
    const std::u8string recordingName = recording.u8string();
    const std::u8string audioName = audio.u8string();
    const std::filesystem::path merged = recording.parent_path() / (recording.stem().u8string() + u8" (merging).mkv");
    const std::u8string mergedName = merged.u8string();

    AVFormatContext* inputs[2] = {};
    AVFormatContext* output = nullptr;
    std::vector<int> streamMaps[2];
    int result = 0;

    auto cleanup = [&]() {
        for (AVFormatContext*& input : inputs) avformat_close_input(&input);

        if (output) {
            if (!(output->oformat->flags & AVFMT_NOFILE)) avio_closep(&output->pb);
            avformat_free_context(output);
            output = nullptr;
        }
    };

    auto fail = [&](const std::string& message) {
        error = message;
        cleanup();
        std::error_code ec;
        std::filesystem::remove(merged, ec);
        return false;
    };

    const char* names[2] = { (const char*)recordingName.c_str(), (const char*)audioName.c_str() };

    for (int i = 0; i < 2; i++) {
        if ((result = avformat_open_input(&inputs[i], names[i], nullptr, nullptr)) < 0 ||
            (result = avformat_find_stream_info(inputs[i], nullptr)) < 0) {
            return fail("Could not read " + std::string(names[i]) + ": " + averror(result));
        }
    }

    if ((result = avformat_alloc_output_context2(&output, nullptr, "matroska", (const char*)mergedName.c_str())) < 0) {
        return fail("Could not create the file: " + averror(result));
    }

    // Every stream of the recording, then the audio of the other file.
    for (int i = 0; i < 2; i++) {
        for (unsigned int s = 0; s < inputs[i]->nb_streams; s++) {
            const AVStream* in = inputs[i]->streams[s];
            const bool copied = (i == 0) || in->codecpar->codec_type == AVMEDIA_TYPE_AUDIO;
            streamMaps[i].push_back(copied ? (int)output->nb_streams : -1);

            if (!copied) {
                continue;
            }

            AVStream* out = avformat_new_stream(output, nullptr);
            avcodec_parameters_copy(out->codecpar, in->codecpar);
            out->codecpar->codec_tag = 0;
            out->time_base = in->time_base;
            av_dict_copy(&out->metadata, in->metadata, 0);

            if (i == 1) {
                av_dict_set(&out->metadata, "title", title.c_str(), 0);
            }
        }
    }

    if ((result = avio_open(&output->pb, (const char*)mergedName.c_str(), AVIO_FLAG_WRITE)) < 0 ||
        (result = avformat_write_header(output, nullptr)) < 0) {
        return fail("Could not write the file: " + averror(result));
    }

    // The packets of both files, by time.
    AVPacket* packets[2] = { av_packet_alloc(), av_packet_alloc() };
    bool pending[2] = {};
    bool ended[2] = {};

    auto next = [&](int i) {
        while (!ended[i] && !pending[i]) {
            if (av_read_frame(inputs[i], packets[i]) < 0) {
                ended[i] = true;
            }
            else if (streamMaps[i][(size_t)packets[i]->stream_index] >= 0) {
                pending[i] = true;
            }
            else {
                av_packet_unref(packets[i]);
            }
        }
    };

    auto timeOf = [&](int i) {
        const AVPacket* packet = packets[i];
        const int64_t t = (packet->dts != AV_NOPTS_VALUE) ? packet->dts : packet->pts;
        return av_rescale_q(t == AV_NOPTS_VALUE ? 0 : t, inputs[i]->streams[packet->stream_index]->time_base, AV_TIME_BASE_Q);
    };

    while (result >= 0) {
        next(0);
        next(1);

        if (!pending[0] && !pending[1]) {
            break;
        }

        const int i = (!pending[1] || (pending[0] && timeOf(0) <= timeOf(1))) ? 0 : 1;
        AVPacket* packet = packets[i];
        const AVStream* in = inputs[i]->streams[packet->stream_index];

        packet->stream_index = streamMaps[i][(size_t)packet->stream_index];
        av_packet_rescale_ts(packet, in->time_base, output->streams[packet->stream_index]->time_base);
        packet->pos = -1;

        result = av_interleaved_write_frame(output, packet);
        pending[i] = false;
    }

    av_packet_free(&packets[0]);
    av_packet_free(&packets[1]);

    if (result < 0 || (result = av_write_trailer(output)) < 0) {
        return fail("Could not write the file: " + averror(result));
    }

    cleanup();

    std::error_code ec;
    std::filesystem::rename(merged, recording, ec);

    if (ec) {
        std::filesystem::remove(merged, ec);
        error = "Could not replace the recording: " + ec.message();
        return false;
    }

    return true;
}


void excludeFromCapture(bool exclude)
{
#ifdef _WIN32
    // WDA_EXCLUDEFROMCAPTURE, Windows 10 2004 and later.
    const DWORD affinity = exclude ? 0x00000011 : WDA_NONE;

    EnumThreadWindows(GetCurrentThreadId(), [](HWND window, LPARAM data) -> BOOL {
        DWORD current = 0;

        if (GetWindowDisplayAffinity(window, &current) && current != (DWORD)data) {
            SetWindowDisplayAffinity(window, (DWORD)data);
        }

        return TRUE;
    }, (LPARAM)affinity);
#else
    (void)exclude;
#endif
}


// ----------------------------------------------------------------------------
// Video: capture filter graph and encoder
// ----------------------------------------------------------------------------

struct ScreenRecorder::Video
{
    AVFilterGraph* graph = nullptr;
    AVFilterContext* sink = nullptr;
    AVCodecContext* encoder = nullptr;
    AVStream* stream = nullptr;

    // Pictures downloaded from the GPU for a software encoder, or decoded
    // from a camera: converted to NV12.
    bool download = false;
    SwsContext* sws = nullptr;
    AVFrame* converted = nullptr;

    // Camera
    AVFormatContext* input = nullptr;
    AVCodecContext* decoder = nullptr;
    int streamIndex = -1;

    ~Video()
    {
        avcodec_free_context(&encoder);
        avcodec_free_context(&decoder);
        avformat_close_input(&input);
        avfilter_graph_free(&graph);
        sws_freeContext(sws);
        av_frame_free(&converted);
    }
};


// ----------------------------------------------------------------------------
// Audio: AAC encoder, fed by the loopback capture
// ----------------------------------------------------------------------------

struct ScreenRecorder::Audio
{
    AVCodecContext* encoder = nullptr;
    AVStream* stream = nullptr;
    AVAudioFifo* fifo = nullptr;
    SwrContext* swr = nullptr;
    int64_t samplesEncoded = 0;

    ~Audio()
    {
        avcodec_free_context(&encoder);
        av_audio_fifo_free(fifo);
        swr_free(&swr);
    }
};


ScreenRecorder::~ScreenRecorder()
{
    stop();
}


void ScreenRecorder::fail(const std::string& message)
{
    std::scoped_lock lock(_errorMutex);

    if (_error.empty()) {
        _error = message;
    }

    SDL_Log("Screen recording: %s", message.c_str());
}


std::string ScreenRecorder::error() const
{
    std::scoped_lock lock(_errorMutex);
    return _error;
}


int ScreenRecorder::write(void* packet)
{
    std::scoped_lock lock(_writeMutex);
    return av_interleaved_write_frame(_format, static_cast<AVPacket*>(packet));
}


bool ScreenRecorder::start(const std::filesystem::path& path, const ScreenRecordSettings& settings, std::string& error)
{
    stop();

#ifndef _WIN32
    (void)path;
    (void)settings;
    error = "Screen recording is only available on Windows for now.";
    return false;
#else
    _error.clear();
    _stop = false;
    _videoStarted = false;
    _seconds = 0.0;
    _frameRateNum = std::clamp(settings.frameRateNum, 1, 240000);
    _frameRateDen = std::clamp(settings.frameRateDen, 1, 100000);
    _previewSws = nullptr;
    _previewTicks = 0;

    const int maxFrameRate = (_frameRateNum + _frameRateDen - 1) / _frameRateDen;

    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);

    const std::u8string pathName = path.u8string();
    const std::string fileName(pathName.begin(), pathName.end());

    int result = avformat_alloc_output_context2(&_format, nullptr, "matroska", fileName.c_str());

    if (result < 0 || !_format) {
        error = "Could not create the file: " + averror(result);
        return false;
    }

    // Video: the capture of a screen or a window, at a constant frame rate,
    // sizes rounded down to even numbers for the encoders. Hardware encoders
    // take its pictures on the GPU; Media Foundation gets them downloaded.
    // A camera: its pictures decoded, converted to NV12 for any encoder.
    const bool camera = settings.source.kind == CaptureSource::Kind::Camera;
    const std::string capture = captureFilter(settings.source, maxFrameRate, settings.cursor) +
        ":width=-2:height=-2,fps=" + std::to_string(_frameRateNum) + "/" + std::to_string(_frameRateDen);

    struct Candidate
    {
        const char* encoder;
        bool download;
    };

    const Candidate candidates[] = {
        { "h264_nvenc", false },
        { "h264_amf", false },
        { "h264_mf", true },
    };

    std::string lastError = "No H.264 encoder could be opened.";

    for (const Candidate& candidate : candidates) {
        const AVCodec* codec = avcodec_find_encoder_by_name(candidate.encoder);

        if (!codec) {
            continue;
        }

        auto video = std::make_unique<Video>();
        video->download = candidate.download || camera;

        int width = 0;
        int height = 0;
        AVRational timeBase = { _frameRateDen, _frameRateNum };
        AVRational frameRate = { _frameRateNum, _frameRateDen };

        if (camera) {
            if (!openCamera(settings.source, true, video->input, video->decoder, video->streamIndex, lastError)) {
                break;
            }

            const AVStream* stream = video->input->streams[video->streamIndex];
            width = video->decoder->width & ~1;
            height = video->decoder->height & ~1;
            timeBase = { 1, 1000 };

            if (stream->avg_frame_rate.num > 0 && stream->avg_frame_rate.den > 0) {
                frameRate = stream->avg_frame_rate;
            }
        }
        else {
            const std::string description = capture + (candidate.download ? ",hwdownload,format=bgra" : "");
            video->graph = openGraph(description, video->sink, result);

            if (!video->graph) {
                lastError = "Could not capture this source: " + averror(result);
                continue;
            }

            width = av_buffersink_get_w(video->sink);
            height = av_buffersink_get_h(video->sink);
        }

        AVCodecContext* encoder = avcodec_alloc_context3(codec);
        video->encoder = encoder;
        encoder->width = width;
        encoder->height = height;
        encoder->time_base = timeBase;
        encoder->framerate = frameRate;
        encoder->gop_size = 2 * std::max(1, frameRate.num / std::max(1, frameRate.den));    // A key frame every 2 s, to seek in it
        encoder->max_b_frames = 0;
        encoder->bit_rate = std::clamp<int64_t>((int64_t)((double)width * height * av_q2d(frameRate) * 0.12), 4'000'000, 80'000'000);
        encoder->color_range = AVCOL_RANGE_MPEG;
        encoder->colorspace = AVCOL_SPC_BT709;
        encoder->color_primaries = AVCOL_PRI_BT709;
        encoder->color_trc = AVCOL_TRC_BT709;

        if (_format->oformat->flags & AVFMT_GLOBALHEADER) {
            encoder->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        }

        if (video->download) {
            encoder->pix_fmt = AV_PIX_FMT_NV12;
        }
        else {
            AVBufferRef* frames = av_buffersink_get_hw_frames_ctx(video->sink);

            if (!frames) {
                lastError = "The capture gave no GPU pictures.";
                continue;
            }

            encoder->pix_fmt = (AVPixelFormat)av_buffersink_get_format(video->sink);
            encoder->hw_frames_ctx = av_buffer_ref(frames);
        }

        AVDictionary* options = nullptr;

        if (std::string(candidate.encoder) == "h264_nvenc") {
            av_dict_set(&options, "preset", "p4", 0);
            av_dict_set(&options, "tune", "hq", 0);
        }
        else if (std::string(candidate.encoder) == "h264_mf") {
            av_dict_set(&options, "hw_encoding", "1", 0);
        }

        result = avcodec_open2(encoder, codec, &options);
        av_dict_free(&options);

        if (result < 0) {
            lastError = std::string("Could not open ") + candidate.encoder + ": " + averror(result);
            continue;
        }

        if (video->download) {
            video->converted = av_frame_alloc();
            video->converted->format = AV_PIX_FMT_NV12;
            video->converted->width = width;
            video->converted->height = height;
            av_frame_get_buffer(video->converted, 0);
        }

        video->stream = avformat_new_stream(_format, nullptr);
        avcodec_parameters_from_context(video->stream->codecpar, encoder);
        video->stream->time_base = encoder->time_base;
        video->stream->avg_frame_rate = encoder->framerate;

        _encoderName = candidate.encoder;
        _video = video.release();
        break;
    }

    if (!_video) {
        error = lastError;
        close();
        return false;
    }

    // Audio: AAC, stereo, 48 kHz.
    if (settings.systemAudio) {
        const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_AAC);
        auto audio = std::make_unique<Audio>();

        audio->encoder = avcodec_alloc_context3(codec);
        audio->encoder->sample_rate = AudioSampleRate;
        audio->encoder->sample_fmt = AV_SAMPLE_FMT_FLTP;
        audio->encoder->bit_rate = 192000;
        audio->encoder->time_base = { 1, AudioSampleRate };
        av_channel_layout_default(&audio->encoder->ch_layout, 2);

        if (_format->oformat->flags & AVFMT_GLOBALHEADER) {
            audio->encoder->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        }

        if (codec && avcodec_open2(audio->encoder, codec, nullptr) >= 0) {
            audio->stream = avformat_new_stream(_format, nullptr);
            avcodec_parameters_from_context(audio->stream->codecpar, audio->encoder);
            av_dict_set(&audio->stream->metadata, "title", "Computer sound", 0);
            audio->stream->time_base = audio->encoder->time_base;
            audio->fifo = av_audio_fifo_alloc(AV_SAMPLE_FMT_FLTP, 2, 8192);
            _audio = audio.release();
        }
        else {
            SDL_Log("Screen recording: no AAC encoder, recording without sound");
        }
    }

    result = avio_open(&_format->pb, fileName.c_str(), AVIO_FLAG_WRITE);

    if (result >= 0) {
        result = avformat_write_header(_format, nullptr);
    }

    if (result < 0) {
        error = "Could not write the file: " + averror(result);
        close();
        return false;
    }

    SDL_Log("Screen recording: %s, %d x %d at %.3f fps with %s",
        settings.source.name.c_str(), _video->encoder->width, _video->encoder->height, av_q2d(_video->encoder->framerate), _encoderName.c_str());

    _recording = true;
    _videoThread = std::thread(&ScreenRecorder::videoLoop, this);

    if (_audio) {
        _audioThread = std::thread(&ScreenRecorder::audioLoop, this);
    }

    return true;
#endif
}


void ScreenRecorder::videoLoop()
{
    Video& video = *_video;
    AVFrame* frame = av_frame_alloc();
    AVPacket* packet = av_packet_alloc();
    AVPacket* input = av_packet_alloc();
    const AVRational sourceTimeBase = video.input ? video.input->streams[video.streamIndex]->time_base : av_buffersink_get_time_base(video.sink);
    int64_t firstPts = AV_NOPTS_VALUE;
    int64_t lastPts = -1;

    auto drain = [&]() {
        while (avcodec_receive_packet(video.encoder, packet) >= 0) {
            av_packet_rescale_ts(packet, video.encoder->time_base, video.stream->time_base);
            packet->stream_index = video.stream->index;

            if (write(packet) < 0) {
                fail("Could not write the file.");
            }
        }
    };

    // One picture to the encoder.
    auto encode = [&](AVFrame* picture) {
        const int64_t sourcePts = (picture->best_effort_timestamp != AV_NOPTS_VALUE) ? picture->best_effort_timestamp : picture->pts;

        if (firstPts == AV_NOPTS_VALUE) {
            firstPts = sourcePts;
            _videoStarted = true;
        }

        updatePreview(picture);

        AVFrame* in = picture;

        if (video.download) {
            video.sws = sws_getCachedContext(video.sws, picture->width, picture->height, (AVPixelFormat)picture->format,
                video.encoder->width, video.encoder->height, AV_PIX_FMT_NV12, SWS_BILINEAR, nullptr, nullptr, nullptr);
            av_frame_make_writable(video.converted);
            sws_scale(video.sws, picture->data, picture->linesize, 0, picture->height, video.converted->data, video.converted->linesize);
            in = video.converted;
        }

        // Increasing time stamps, whatever the source gives.
        int64_t pts = av_rescale_q(sourcePts - firstPts, sourceTimeBase, video.encoder->time_base);
        pts = std::max(pts, lastPts + 1);
        lastPts = pts;
        in->pts = pts;

        if (avcodec_send_frame(video.encoder, in) >= 0) {
            drain();
        }

        _seconds = (double)pts * av_q2d(video.encoder->time_base) + 1.0 / std::max(1.0, av_q2d(video.encoder->framerate));
    };

    while (!_stop) {
        if (video.input) {
            const int result = av_read_frame(video.input, input);

            if (result == AVERROR(EAGAIN)) {
                SDL_Delay(1);
                continue;
            }

            if (result < 0) {
                fail("The camera stopped: " + averror(result));
                break;
            }

            if (input->stream_index == video.streamIndex && avcodec_send_packet(video.decoder, input) >= 0) {
                while (avcodec_receive_frame(video.decoder, frame) >= 0) {
                    encode(frame);
                    av_frame_unref(frame);
                }
            }

            av_packet_unref(input);
            continue;
        }

        const int result = av_buffersink_get_frame(video.sink, frame);

        if (result == AVERROR(EAGAIN)) {
            SDL_Delay(1);
            continue;
        }

        if (result < 0) {
            // The window closed, most often.
            fail("The capture stopped: " + averror(result));
            break;
        }

        encode(frame);
        av_frame_unref(frame);
    }

    avcodec_send_frame(video.encoder, nullptr);
    drain();

    av_packet_free(&input);
    av_packet_free(&packet);
    av_frame_free(&frame);

    _recording = false;
}


void ScreenRecorder::audioLoop()
{
#ifdef _WIN32
    Audio& audio = *_audio;
    AVPacket* packet = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    frame->format = AV_SAMPLE_FMT_FLTP;
    frame->sample_rate = AudioSampleRate;
    frame->nb_samples = audio.encoder->frame_size;
    av_channel_layout_default(&frame->ch_layout, 2);
    av_frame_get_buffer(frame, 0);

    // Loopback capture of the default output.
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    IMMDeviceEnumerator* enumerator = nullptr;
    IMMDevice* device = nullptr;
    IAudioClient* client = nullptr;
    IAudioCaptureClient* captureClient = nullptr;
    WAVEFORMATEX* format = nullptr;

    bool captured = SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&enumerator))
        && SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device))
        && SUCCEEDED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&client))
        && SUCCEEDED(client->GetMixFormat(&format))
        && SUCCEEDED(client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, 2'000'000, 0, format, nullptr))
        && SUCCEEDED(client->GetService(__uuidof(IAudioCaptureClient), (void**)&captureClient))
        && SUCCEEDED(client->Start());

    // Format of the samples captured: silence of 48 kHz stereo floats without
    // a device.
    int inputRate = AudioSampleRate;
    int inputChannels = 2;
    AVSampleFormat inputFormat = AV_SAMPLE_FMT_FLT;

    if (captured) {
        inputRate = (int)format->nSamplesPerSec;
        inputChannels = format->nChannels;

        bool isFloat = format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT;

        if (format->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
            isFloat = reinterpret_cast<WAVEFORMATEXTENSIBLE*>(format)->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
        }

        inputFormat = isFloat ? AV_SAMPLE_FMT_FLT : (format->wBitsPerSample == 16 ? AV_SAMPLE_FMT_S16 : AV_SAMPLE_FMT_S32);
    }
    else {
        SDL_Log("Screen recording: no loopback capture of the computer sound, recording silence");
    }

    AVChannelLayout inputLayout = {};
    av_channel_layout_default(&inputLayout, inputChannels);
    swr_alloc_set_opts2(&audio.swr, &audio.encoder->ch_layout, AV_SAMPLE_FMT_FLTP, AudioSampleRate, &inputLayout, inputFormat, inputRate, 0, nullptr);
    swr_init(audio.swr);
    av_channel_layout_uninit(&inputLayout);

    const int bytesPerFrame = inputChannels * av_get_bytes_per_sample(inputFormat);
    std::vector<uint8_t> silence;
    std::vector<float> converted[2];

    auto encodeAvailable = [&](bool flush) {
        while (av_audio_fifo_size(audio.fifo) >= frame->nb_samples || (flush && av_audio_fifo_size(audio.fifo) > 0)) {
            av_frame_make_writable(frame);
            const int count = av_audio_fifo_read(audio.fifo, (void**)frame->data, frame->nb_samples);

            // The last frame padded with silence.
            for (int c = 0; c < 2; c++) {
                std::fill((float*)frame->data[c] + count, (float*)frame->data[c] + frame->nb_samples, 0.f);
            }

            frame->pts = audio.samplesEncoded;
            audio.samplesEncoded += frame->nb_samples;

            if (avcodec_send_frame(audio.encoder, frame) < 0) {
                break;
            }

            while (avcodec_receive_packet(audio.encoder, packet) >= 0) {
                av_packet_rescale_ts(packet, audio.encoder->time_base, audio.stream->time_base);
                packet->stream_index = audio.stream->index;
                write(packet);
            }
        }
    };

    // Samples in the format of the capture, to 48 kHz planar floats.
    auto feed = [&](const uint8_t* data, int frames) {
        const int outCount = swr_get_out_samples(audio.swr, frames);

        for (auto& channel : converted) {
            channel.resize((size_t)std::max(outCount, 1));
        }

        uint8_t* out[2] = { (uint8_t*)converted[0].data(), (uint8_t*)converted[1].data() };
        const uint8_t* in[1] = { data };
        const int count = swr_convert(audio.swr, out, outCount, in, frames);

        if (count <= 0) {
            return;
        }

        float peak = 0.f;

        for (int i = 0; i < count; i++) {
            peak = std::max({ peak, std::fabs(converted[0][i]), std::fabs(converted[1][i]) });
        }

        float previous = _audioPeak.load();
        while (peak > previous && !_audioPeak.compare_exchange_weak(previous, peak)) {}

        av_audio_fifo_write(audio.fifo, (void**)out, count);
        encodeAvailable(false);
    };

    auto feedSilence = [&](int64_t frames) {
        while (frames > 0) {
            const int chunk = (int)std::min<int64_t>(frames, 4800);
            silence.assign((size_t)chunk * bytesPerFrame, 0);
            feed(silence.data(), chunk);
            frames -= chunk;
        }
    };

    // The sound from the first picture on (a camera takes a moment to give
    // it), to start together.
    while (!_stop && !_videoStarted) {
        UINT32 packetSize = 0;

        while (captured && SUCCEEDED(captureClient->GetNextPacketSize(&packetSize)) && packetSize > 0) {
            BYTE* data = nullptr;
            UINT32 frames = 0;
            DWORD flags = 0;

            if (FAILED(captureClient->GetBuffer(&data, &frames, &flags, nullptr, nullptr))) {
                break;
            }

            captureClient->ReleaseBuffer(frames);
        }

        SDL_Delay(2);
    }

    const auto startTime = std::chrono::steady_clock::now();
    int64_t inputFrames = 0;

    while (!_stop) {
        SDL_Delay(10);

        UINT32 packetSize = 0;

        while (captured && SUCCEEDED(captureClient->GetNextPacketSize(&packetSize)) && packetSize > 0) {
            BYTE* data = nullptr;
            UINT32 frames = 0;
            DWORD flags = 0;

            if (FAILED(captureClient->GetBuffer(&data, &frames, &flags, nullptr, nullptr))) {
                break;
            }

            if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                feedSilence(frames);
            }
            else {
                feed(data, (int)frames);
            }

            inputFrames += frames;
            captureClient->ReleaseBuffer(frames);
        }

        // Nothing comes while the computer plays nothing: silence, to stay in
        // time with the pictures.
        const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - startTime).count();
        const int64_t expected = (int64_t)(elapsed * inputRate);
        const int64_t late = expected - inputFrames - inputRate / 20;

        if (late > 0) {
            feedSilence(late);
            inputFrames += late;
        }
    }

    encodeAvailable(true);
    avcodec_send_frame(audio.encoder, nullptr);

    while (avcodec_receive_packet(audio.encoder, packet) >= 0) {
        av_packet_rescale_ts(packet, audio.encoder->time_base, audio.stream->time_base);
        packet->stream_index = audio.stream->index;
        write(packet);
    }

    if (client) client->Stop();
    if (captureClient) captureClient->Release();
    if (client) client->Release();
    if (device) device->Release();
    if (enumerator) enumerator->Release();
    CoTaskMemFree(format);
    CoUninitialize();

    av_frame_free(&frame);
    av_packet_free(&packet);
#endif
}


void ScreenRecorder::updatePreview(const void* data)
{
    // A few times a second; pictures on the GPU downloaded for it.
    const uint64_t now = SDL_GetTicks();

    if (now - _previewTicks < 150) {
        return;
    }

    _previewTicks = now;

    const AVFrame* picture = static_cast<const AVFrame*>(data);
    AVFrame* downloaded = nullptr;

    if (picture->hw_frames_ctx) {
        downloaded = av_frame_alloc();

        if (av_hwframe_transfer_data(downloaded, picture, 0) < 0) {
            av_frame_free(&downloaded);
            return;
        }

        picture = downloaded;
    }

    std::vector<uint8_t> rgba((size_t)SourcePreview::Width * SourcePreview::Height * 4);
    SwsContext* sws = static_cast<SwsContext*>(_previewSws);
    toPreview(sws, picture, rgba, SourcePreview::Width, SourcePreview::Height);
    _previewSws = sws;

    av_frame_free(&downloaded);

    std::scoped_lock lock(_previewMutex);
    _preview.swap(rgba);
    _previewFresh = true;
}


bool ScreenRecorder::takePreview(std::vector<uint8_t>& rgba)
{
    std::scoped_lock lock(_previewMutex);

    if (!_previewFresh) {
        return false;
    }

    _previewFresh = false;
    rgba = _preview;

    return true;
}


bool ScreenRecorder::stop()
{
    if (!_format) {
        return false;
    }

    _stop = true;

    if (_videoThread.joinable()) _videoThread.join();
    if (_audioThread.joinable()) _audioThread.join();

    if (_format->pb) {
        av_write_trailer(_format);
    }

    sws_freeContext(static_cast<SwsContext*>(_previewSws));
    _previewSws = nullptr;

    close();

    return error().empty();
}


void ScreenRecorder::close()
{
    _recording = false;

    delete _video;
    _video = nullptr;
    delete _audio;
    _audio = nullptr;

    if (_format) {
        if (_format->pb) {
            avio_closep(&_format->pb);
        }

        avformat_free_context(_format);
        _format = nullptr;
    }
}


// ----------------------------------------------------------------------------
// Source preview
// ----------------------------------------------------------------------------

struct SourcePreview::State
{
    std::atomic<bool> stop = false;
    std::atomic<bool> done = false;

    std::mutex mutex;
    std::vector<uint8_t> rgba = std::vector<uint8_t>((size_t)Width * Height * 4, 0);
    bool fresh = false;
    std::string error;
};


namespace {

void previewLoop(std::shared_ptr<SourcePreview::State> state, CaptureSource source)
{
    AVFrame* frame = av_frame_alloc();
    AVPacket* packet = av_packet_alloc();
    SwsContext* sws = nullptr;
    std::vector<uint8_t> rgba((size_t)SourcePreview::Width * SourcePreview::Height * 4);

    AVFilterGraph* graph = nullptr;
    AVFilterContext* sink = nullptr;
    AVFormatContext* input = nullptr;
    AVCodecContext* decoder = nullptr;
    int streamIndex = -1;
    std::string error;

    auto publish = [&]() {
        toPreview(sws, frame, rgba, SourcePreview::Width, SourcePreview::Height);

        std::scoped_lock lock(state->mutex);
        state->rgba.swap(rgba);
        state->fresh = true;
    };

    bool opened = false;

    if (source.kind == CaptureSource::Kind::Camera) {
        opened = openCamera(source, false, input, decoder, streamIndex, error);
    }
    else {
        int result = 0;
        graph = openGraph(captureFilter(source, 5, true) + ",hwdownload,format=bgra", sink, result);
        opened = graph != nullptr;

        if (!opened) {
            error = "No picture from this source: " + averror(result);
        }
    }

    if (!opened) {
        std::scoped_lock lock(state->mutex);
        state->error = error;
    }

    while (opened && !state->stop) {
        if (input) {
            const int result = av_read_frame(input, packet);

            if (result == AVERROR(EAGAIN)) {
                SDL_Delay(5);
                continue;
            }

            if (result < 0) {
                break;
            }

            // A few pictures a second are enough.
            static thread_local Uint64 last = 0;

            if (packet->stream_index == streamIndex && avcodec_send_packet(decoder, packet) >= 0) {
                while (avcodec_receive_frame(decoder, frame) >= 0) {
                    if (SDL_GetTicks() - last > 150) {
                        last = SDL_GetTicks();
                        publish();
                    }

                    av_frame_unref(frame);
                }
            }

            av_packet_unref(packet);
            continue;
        }

        const int result = av_buffersink_get_frame(sink, frame);

        if (result == AVERROR(EAGAIN)) {
            SDL_Delay(5);
            continue;
        }

        if (result < 0) {
            break;
        }

        publish();
        av_frame_unref(frame);
    }

    avfilter_graph_free(&graph);
    avcodec_free_context(&decoder);
    avformat_close_input(&input);
    sws_freeContext(sws);
    av_packet_free(&packet);
    av_frame_free(&frame);

    state->done = true;
}

} // namespace


void SourcePreview::show(const CaptureSource& source)
{
    if (_state && source == _source) {
        return;
    }

    // Another format of the same camera: released first, it opens once.
    stop(_state && source.kind == CaptureSource::Kind::Camera && source.sameDevice(_source));
    _source = source;

    if (!source.handle) {
        return;
    }

    // The thread owns the state with this object: stopping never waits for
    // a capture that has nothing new to give.
    _state = std::make_shared<State>();
    std::thread(previewLoop, _state, source).detach();
}


void SourcePreview::stop(bool wait)
{
    if (!_state) {
        return;
    }

    _state->stop = true;

    for (int i = 0; wait && i < 300 && !_state->done; i++) {
        SDL_Delay(10);
    }

    _state.reset();
    _source = CaptureSource();
}


bool SourcePreview::take(std::vector<uint8_t>& rgba)
{
    if (!_state) {
        return false;
    }

    std::scoped_lock lock(_state->mutex);

    if (!_state->fresh) {
        return false;
    }

    _state->fresh = false;
    rgba = _state->rgba;

    return true;
}


std::string SourcePreview::error() const
{
    if (!_state) {
        return {};
    }

    std::scoped_lock lock(_state->mutex);
    return _state->error;
}

}

#include <app/RecordingScene.h>

#include <core/Json.h>
#include <core/Paths.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>


using namespace core;


namespace {

std::filesystem::path scenesPath()
{
    return configDirectory() / "scenes.json";
}


const char* kindName(capture::CaptureSource::Kind kind)
{
    switch (kind) {
        case capture::CaptureSource::Kind::Window: return "window";
        case capture::CaptureSource::Kind::Camera: return "camera";
        default:                                   return "screen";
    }
}

} // namespace


std::vector<RecordingScene> loadScenes(const std::vector<capture::CaptureSource>& available)
{
    std::vector<RecordingScene> scenes;
    std::ifstream file(scenesPath(), std::ios::binary);

    if (!file) {
        return scenes;
    }

    std::stringstream text;
    text << file.rdbuf();
    Json json;

    try {
        json = Json::parse(text.str());
    }
    catch (const std::exception&) {
        return scenes;
    }

    for (const Json& entry : json["scenes"].items()) {
        RecordingScene& scene = scenes.emplace_back();
        scene.name = entry["name"].toString();

        for (const Json& item : entry["sources"].items()) {
            SceneSource source;
            const std::string kind = item["kind"].toString();
            source.source.kind = (kind == "window") ? capture::CaptureSource::Kind::Window
                : (kind == "camera") ? capture::CaptureSource::Kind::Camera : capture::CaptureSource::Kind::Monitor;
            source.source.name = item["name"].toString();
            source.source.device = item["device"].toString();
            if (item.contains("scale")) {
                const std::string fit = item["fit"].toString();
                source.fit = (fit == "fill") ? core::FitMode::Fill : (fit == "native") ? core::FitMode::Native : core::FitMode::Fit;
                source.x = (float)item["x"].toDouble(0.0);
                source.y = (float)item["y"].toDouble(0.0);
                source.scale = (float)std::max(0.01, item["scale"].toDouble(1.0));
                source.rotation = (float)item["rotation"].toDouble(0.0);
            }
            else {
                // Saved as a rectangle before.
                placeInRectangle(source, (float)item["x"].toDouble(0.0), (float)item["y"].toDouble(0.0),
                    (float)std::max(0.01, item["width"].toDouble(1.0)), (float)std::max(0.01, item["height"].toDouble(1.0)));
            }

            source.cropLeft = (float)std::clamp(item["cropLeft"].toDouble(0.0), 0.0, 0.98);
            source.cropTop = (float)std::clamp(item["cropTop"].toDouble(0.0), 0.0, 0.98);
            source.cropRight = (float)std::clamp(item["cropRight"].toDouble(0.0), 0.0, 0.98 - source.cropLeft);
            source.cropBottom = (float)std::clamp(item["cropBottom"].toDouble(0.0), 0.0, 0.98 - source.cropTop);

            const Json& format = item["format"];
            source.source.format.width = (int)format["width"].toInt(0);
            source.source.format.height = (int)format["height"].toInt(0);
            source.source.format.codec = format["codec"].toString();
            source.source.format.pixelFormat = format["pixelFormat"].toString();
            source.source.frameRate = format["frameRate"].toDouble(0.0);

            // The same source now: its handle (windows and screens change
            // from one session to the other).
            const auto found = std::find_if(available.begin(), available.end(), [&](const capture::CaptureSource& candidate) {
                return candidate.kind == source.source.kind &&
                    (candidate.kind == capture::CaptureSource::Kind::Camera ? candidate.device == source.source.device
                                                                           : candidate.name == source.source.name);
            });

            source.available = found != available.end();

            if (source.available) {
                source.source.handle = found->handle;
                source.source.device = found->device;
            }

            scene.sources.push_back(std::move(source));
        }
    }

    return scenes;
}


void placeInRectangle(SceneSource& source, float x, float y, float width, float height)
{
    // The rectangle had the shape of the picture: fitted in the frame, the
    // picture is as wide as the frame or as high, so the scale is the
    // larger of the two fractions.
    source.fit = core::FitMode::Fit;
    source.scale = std::max(0.01f, std::max(width, height));
    source.rotation = 0.f;
    source.x = x + 0.5f * width - 0.5f;
    source.y = y + 0.5f * height - 0.5f;
}


ImVec2 sourceHalfSize(const SceneSource& source, float frameWidth, float frameHeight, int pictureWidth, int pictureHeight)
{
    const float width = (pictureWidth > 0 && pictureHeight > 0) ? (float)pictureWidth : 1920.f;
    const float height = (pictureWidth > 0 && pictureHeight > 0) ? (float)pictureHeight : 1080.f;
    const float fit = core::fitScale(source.fit, frameWidth, frameHeight, width, height);

    return ImVec2(0.5f * width * fit, 0.5f * height * fit);
}


std::array<ImVec2, 4> sourceCorners(const SceneSource& source, float frameWidth, float frameHeight, int pictureWidth, int pictureHeight)
{
    const ImVec2 half = sourceHalfSize(source, frameWidth, frameHeight, pictureWidth, pictureHeight);
    const float left = -half.x + 2.f * half.x * source.cropLeft;
    const float right = half.x - 2.f * half.x * source.cropRight;
    const float top = -half.y + 2.f * half.y * source.cropTop;
    const float bottom = half.y - 2.f * half.y * source.cropBottom;
    const float angle = source.rotation * 3.14159265358979f / 180.f;
    const float cosine = std::cos(angle);
    const float sine = std::sin(angle);
    const float centerX = 0.5f * frameWidth + source.x * frameWidth;
    const float centerY = 0.5f * frameHeight + source.y * frameHeight;

    auto place = [&](float px, float py) {
        const float sx = px * source.scale;
        const float sy = py * source.scale;
        return ImVec2(centerX + sx * cosine - sy * sine, centerY + sx * sine + sy * cosine);
    };

    return { place(left, top), place(right, top), place(right, bottom), place(left, bottom) };
}


void saveScenes(const std::vector<RecordingScene>& scenes)
{
    Json list = Json::array();

    for (const RecordingScene& scene : scenes) {
        Json entry = Json::object();
        entry["name"] = scene.name;
        Json sources = Json::array();

        for (const SceneSource& source : scene.sources) {
            Json item = Json::object();
            item["kind"] = kindName(source.source.kind);
            item["name"] = source.source.name;
            item["device"] = source.source.device;
            item["fit"] = source.fit == core::FitMode::Fill ? "fill" : source.fit == core::FitMode::Native ? "native" : "fit";
            item["x"] = (double)source.x;
            item["y"] = (double)source.y;
            item["scale"] = (double)source.scale;
            item["rotation"] = (double)source.rotation;
            item["cropLeft"] = (double)source.cropLeft;
            item["cropTop"] = (double)source.cropTop;
            item["cropRight"] = (double)source.cropRight;
            item["cropBottom"] = (double)source.cropBottom;

            if (!source.source.format.automatic()) {
                Json format = Json::object();
                format["width"] = source.source.format.width;
                format["height"] = source.source.format.height;
                format["codec"] = source.source.format.codec;
                format["pixelFormat"] = source.source.format.pixelFormat;
                format["frameRate"] = source.source.frameRate;
                item["format"] = format;
            }

            sources.push_back(item);
        }

        entry["sources"] = sources;
        list.push_back(entry);
    }

    Json json = Json::object();
    json["scenes"] = list;
    std::ofstream(scenesPath(), std::ios::binary) << json.dump();
}

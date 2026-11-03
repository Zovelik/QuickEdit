#include <app/RecordingScene.h>

#include <core/Json.h>
#include <core/Paths.h>

#include <algorithm>
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
            source.x = (float)item["x"].toDouble(0.0);
            source.y = (float)item["y"].toDouble(0.0);
            source.width = (float)std::max(0.01, item["width"].toDouble(1.0));
            source.height = (float)std::max(0.01, item["height"].toDouble(1.0));
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
            item["x"] = (double)source.x;
            item["y"] = (double)source.y;
            item["width"] = (double)source.width;
            item["height"] = (double)source.height;
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

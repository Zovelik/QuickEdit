#include <app/I18n.h>

#include <core/Json.h>

#include <SDL3/SDL.h>

#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <unordered_map>


namespace i18n
{

namespace {

std::mutex mutex;
std::string current = "en";
std::unordered_map<std::string, std::string> catalog;

// Translated labels, by the label given (icons and ID included).
std::unordered_map<std::string, std::string> translated;


std::filesystem::path languageFolder()
{
    const char* base = SDL_GetBasePath();
    return std::filesystem::u8path(base ? base : "") / "lang";
}


bool readCatalog(const std::filesystem::path& path, core::Json& json)
{
    std::ifstream file(path, std::ios::binary);

    if (!file) {
        return false;
    }

    std::stringstream text;
    text << file.rdbuf();

    try {
        json = core::Json::parse(text.str());
        return true;
    }
    catch (const std::exception& e) {
        SDL_Log("Language %s: %s", path.string().c_str(), e.what());
        return false;
    }
}


// Length of the icons (and spaces after them) at the start of a label:
// characters of the Private Use Area, U+E000 to U+F8FF, three bytes each.
size_t iconPrefix(const std::string& text)
{
    size_t i = 0;

    while (i + 3 <= text.size()) {
        const unsigned char c = (unsigned char)text[i];

        if (c != 0xEE && c != 0xEF) {
            break;
        }

        i += 3;

        while (i < text.size() && text[i] == ' ') {
            i++;
        }
    }

    return i;
}

} // namespace


std::vector<Language> languages()
{
    std::vector<Language> list = { { "en", "English" } };
    std::error_code error;

    for (const auto& entry : std::filesystem::directory_iterator(languageFolder(), error)) {
        if (entry.path().extension() != ".json") {
            continue;
        }

        core::Json json;

        if (readCatalog(entry.path(), json)) {
            const std::u8string code = entry.path().stem().u8string();
            list.push_back({ std::string(code.begin(), code.end()), json["_language"].toString() });
        }
    }

    return list;
}


bool setLanguage(const std::string& code)
{
    std::unordered_map<std::string, std::string> loaded;

    if (code != "en") {
        core::Json json;

        if (!readCatalog(languageFolder() / std::filesystem::u8path(code + ".json"), json)) {
            return false;
        }

        for (const auto& [key, value] : json.members()) {
            if (key.empty() || key[0] == '_') {
                continue;
            }

            if (const std::string text = value.toString(); !text.empty()) {
                loaded[key] = text;
            }
        }
    }

    std::scoped_lock lock(mutex);
    current = code;
    catalog = std::move(loaded);
    translated.clear();
    return true;
}


const std::string& language()
{
    return current;
}


std::string systemLanguage()
{
    int count = 0;
    SDL_Locale** locales = SDL_GetPreferredLocales(&count);
    std::string found = "en";

    if (locales) {
        const std::vector<Language> available = languages();

        for (int i = 0; i < count && found == "en"; i++) {
            for (const Language& language : available) {
                if (locales[i]->language && language.code == locales[i]->language) {
                    found = language.code;
                    break;
                }
            }
        }

        SDL_free(locales);
    }

    return found;
}


const char* tr(const char* text)
{
    if (!text || !*text) {
        return text;
    }

    std::scoped_lock lock(mutex);

    if (catalog.empty()) {
        return text;
    }

    const std::string label = text;

    if (const auto it = translated.find(label); it != translated.end()) {
        return it->second.c_str();
    }

    // Icons + text + ID: only the text is translated, without its spaces
    // at the ends.
    const size_t start = iconPrefix(label);
    const size_t idStart = label.find("##", start);
    const size_t end = (idStart == std::string::npos) ? label.size() : idStart;

    size_t first = start;
    size_t last = end;

    while (first < last && label[first] == ' ') first++;
    while (last > first && label[last - 1] == ' ') last--;

    std::string result = label;

    if (const auto it = catalog.find(label.substr(first, last - first)); it != catalog.end()) {
        result = label.substr(0, first) + it->second + label.substr(last);
    }

    return translated.emplace(label, std::move(result)).first->second.c_str();
}


std::string tr(const std::string& text)
{
    return tr(text.c_str());
}

}

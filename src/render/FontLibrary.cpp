#include <render/FontLibrary.h>

// The stb_truetype copy shipped with Dear ImGui, compiled here with static
// linkage as imgui_draw.cpp does.
#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include <imstb_truetype.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <set>
#include <stdexcept>


namespace {

// Distance field encoding: 128 on the outline, PixelDistScale per pixel.
constexpr unsigned char OnEdgeValue = 128;
constexpr float PixelDistScale = 128.f / (float)FontLibrary::Padding;


std::vector<uint8_t> readFile(const std::filesystem::path& path, size_t offset = 0, size_t size = 0)
{
    std::ifstream file(path, std::ios::binary);

    if (!file) {
        return {};
    }

    if (size == 0) {
        file.seekg(0, std::ios::end);
        size = (size_t)file.tellg() - offset;
    }

    std::vector<uint8_t> data(size);
    file.seekg((std::streamoff)offset);
    file.read((char*)data.data(), (std::streamsize)size);

    if ((size_t)file.gcount() != size) {
        return {};
    }

    return data;
}


uint16_t readU16(const uint8_t* p) { return (uint16_t)((p[0] << 8) | p[1]); }
uint32_t readU32(const uint8_t* p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }


void appendUtf8(std::string& out, uint32_t codepoint)
{
    if (codepoint < 0x80) {
        out += (char)codepoint;
    }
    else if (codepoint < 0x800) {
        out += (char)(0xC0 | (codepoint >> 6));
        out += (char)(0x80 | (codepoint & 0x3F));
    }
    else {
        out += (char)(0xE0 | (codepoint >> 12));
        out += (char)(0x80 | ((codepoint >> 6) & 0x3F));
        out += (char)(0x80 | (codepoint & 0x3F));
    }
}


// Reads the family and style names of a font file from its 'name' table,
// without loading the whole file: system fonts weigh hundreds of megabytes.
bool readFontNames(const std::filesystem::path& path, std::string& family, std::string& style)
{
    std::vector<uint8_t> header = readFile(path, 0, 12);

    if (header.size() < 12) {
        return false;
    }

    size_t fontOffset = 0;

    // Font collection: the first font.
    if (std::memcmp(header.data(), "ttcf", 4) == 0) {
        fontOffset = readU32(header.data() + 8 + 4);
        header = readFile(path, fontOffset, 12);

        if (header.size() < 12) {
            return false;
        }
    }

    const uint16_t tableCount = readU16(header.data() + 4);
    const std::vector<uint8_t> tables = readFile(path, fontOffset + 12, (size_t)tableCount * 16);

    if (tables.size() < (size_t)tableCount * 16) {
        return false;
    }

    for (uint16_t i = 0; i < tableCount; i++) {
        const uint8_t* entry = tables.data() + i * 16;

        if (std::memcmp(entry, "name", 4) != 0) {
            continue;
        }

        const std::vector<uint8_t> name = readFile(path, readU32(entry + 8), readU32(entry + 12));

        if (name.size() < 6) {
            return false;
        }

        const uint16_t count = readU16(name.data() + 2);
        const uint16_t stringsOffset = readU16(name.data() + 4);

        // Typographic names (16, 17) win over the legacy ones (1, 2), which
        // split large families in groups of four styles.
        std::string names[18];

        for (uint16_t r = 0; r < count && 6 + (size_t)(r + 1) * 12 <= name.size(); r++) {
            const uint8_t* record = name.data() + 6 + r * 12;
            const uint16_t platform = readU16(record);
            const uint16_t language = readU16(record + 4);
            const uint16_t nameId = readU16(record + 6);
            const uint16_t length = readU16(record + 8);
            const size_t offset = (size_t)stringsOffset + readU16(record + 10);

            if (nameId >= 18 || !names[nameId].empty() || offset + length > name.size()) {
                continue;
            }

            std::string text;

            if (platform == 3 && (language == 0x409 || language == 0)) {
                // UTF-16 big endian
                for (size_t c = 0; c + 1 < length; c += 2) {
                    appendUtf8(text, readU16(name.data() + offset + c));
                }
            }
            else if (platform == 1 && language == 0) {
                text.assign((const char*)name.data() + offset, length);
            }

            names[nameId] = text;
        }

        family = !names[16].empty() ? names[16] : names[1];
        style = !names[17].empty() ? names[17] : names[2];

        return !family.empty();
    }

    return false;
}


std::vector<std::filesystem::path> systemFontDirectories()
{
    std::vector<std::filesystem::path> directories;

#ifdef _WIN32
    if (const char* windows = std::getenv("WINDIR")) {
        directories.push_back(std::filesystem::path(windows) / "Fonts");
    }

    if (const char* local = std::getenv("LOCALAPPDATA")) {
        directories.push_back(std::filesystem::path(local) / "Microsoft" / "Windows" / "Fonts");
    }
#else
    directories.push_back("/usr/share/fonts");
    directories.push_back("/usr/local/share/fonts");

    if (const char* home = std::getenv("HOME")) {
        directories.push_back(std::filesystem::path(home) / ".local" / "share" / "fonts");
        directories.push_back(std::filesystem::path(home) / ".fonts");
    }
#endif

    return directories;
}

} // namespace


struct FontLibrary::Font
{
    std::vector<uint8_t> data;
    stbtt_fontinfo info;
    float scale = 1.f;          // Font units to pixels at BaseSize
    Metrics metrics;
    std::map<uint32_t, Glyph> glyphs;
};


FontLibrary::FontLibrary(std::vector<uint8_t> defaultFont, bool scanSystemFonts)
    : _atlas((size_t)AtlasSize * AtlasSize, 0)
{
    auto font = std::make_unique<Font>();
    font->data = std::move(defaultFont);

    if (!stbtt_InitFont(&font->info, font->data.data(), stbtt_GetFontOffsetForIndex(font->data.data(), 0))) {
        throw std::runtime_error("Could not load the default font");
    }

    _fonts.push_back(std::move(font));
    _fontByPath[""] = 0;

    // Load the metrics of the default font.
    Font& f = *_fonts[0];
    f.scale = stbtt_ScaleForMappingEmToPixels(&f.info, BaseSize);

    int ascent, descent, lineGap;
    stbtt_GetFontVMetrics(&f.info, &ascent, &descent, &lineGap);
    f.metrics = { ascent * f.scale, descent * f.scale, lineGap * f.scale };

    if (scanSystemFonts) {
        _scanThread = std::thread([this]() { this->scanSystemFonts(); });
    }
}


FontLibrary::~FontLibrary()
{
    if (_scanThread.joinable()) {
        _scanThread.join();
    }
}


int FontLibrary::font(const std::string& path)
{
    const auto it = _fontByPath.find(path);

    if (it != _fontByPath.end()) {
        return it->second;
    }

    auto font = std::make_unique<Font>();
    font->data = readFile(std::filesystem::path(std::u8string(path.begin(), path.end())));

    const int offset = font->data.empty() ? -1 : stbtt_GetFontOffsetForIndex(font->data.data(), 0);

    if (offset < 0 || !stbtt_InitFont(&font->info, font->data.data(), offset)) {
        // Unreadable: the default font, from now on.
        _fontByPath[path] = 0;
        return 0;
    }

    font->scale = stbtt_ScaleForMappingEmToPixels(&font->info, BaseSize);

    int ascent, descent, lineGap;
    stbtt_GetFontVMetrics(&font->info, &ascent, &descent, &lineGap);
    font->metrics = { ascent * font->scale, descent * font->scale, lineGap * font->scale };

    _fonts.push_back(std::move(font));
    const int index = (int)_fonts.size() - 1;
    _fontByPath[path] = index;

    return index;
}


const FontLibrary::Metrics& FontLibrary::metrics(int font) const
{
    return _fonts[font]->metrics;
}


float FontLibrary::kerning(int font, uint32_t left, uint32_t right)
{
    Font& f = *_fonts[font];
    return stbtt_GetCodepointKernAdvance(&f.info, (int)left, (int)right) * f.scale;
}


bool FontLibrary::allocate(uint16_t width, uint16_t height, uint16_t& x, uint16_t& y)
{
    if (width > AtlasSize || height > AtlasSize) {
        return false;
    }

    // Next shelf when the row is full.
    if (_shelfX + width > AtlasSize) {
        _shelfY += _shelfHeight + 1;
        _shelfX = 0;
        _shelfHeight = 0;
    }

    if (_shelfY + height > AtlasSize) {
        return false;
    }

    x = _shelfX;
    y = _shelfY;

    _shelfX += width + 1;
    _shelfHeight = std::max(_shelfHeight, height);

    return true;
}


void FontLibrary::clearAtlas()
{
    std::fill(_atlas.begin(), _atlas.end(), (uint8_t)0);

    for (const std::unique_ptr<Font>& font : _fonts) {
        font->glyphs.clear();
    }

    _shelfX = _shelfY = _shelfHeight = 0;
    _dirty = true;
    _generation++;
}


const FontLibrary::Glyph& FontLibrary::glyph(int font, uint32_t codepoint)
{
    Font& f = *_fonts[font];

    const auto it = f.glyphs.find(codepoint);

    if (it != f.glyphs.end()) {
        return it->second;
    }

    const int glyphIndex = stbtt_FindGlyphIndex(&f.info, (int)codepoint);

    // Missing in this font: the default font has it, or nothing does.
    if (glyphIndex == 0 && font != 0 && codepoint > ' ') {
        return glyph(0, codepoint);
    }

    Glyph glyph;

    int advance, leftBearing;
    stbtt_GetGlyphHMetrics(&f.info, glyphIndex, &advance, &leftBearing);
    glyph.advance = advance * f.scale;

    int width = 0, height = 0, offsetX = 0, offsetY = 0;
    unsigned char* sdf = stbtt_GetGlyphSDF(&f.info, f.scale, glyphIndex, Padding, OnEdgeValue, PixelDistScale,
        &width, &height, &offsetX, &offsetY);

    if (sdf) {
        uint16_t x, y;

        if (!allocate((uint16_t)width, (uint16_t)height, x, y)) {
            // Full: start over. The glyphs of this frame are rendered again.
            stbtt_FreeSDF(sdf, nullptr);
            clearAtlas();
            return this->glyph(font, codepoint);
        }

        for (int row = 0; row < height; row++) {
            std::memcpy(&_atlas[(size_t)(y + row) * AtlasSize + x], sdf + (size_t)row * width, (size_t)width);
        }

        stbtt_FreeSDF(sdf, nullptr);

        glyph.x = x;
        glyph.y = y;
        glyph.width = (uint16_t)width;
        glyph.height = (uint16_t)height;
        glyph.offsetX = (float)offsetX;
        glyph.offsetY = (float)offsetY;
        glyph.empty = false;
        _dirty = true;
    }

    return f.glyphs.emplace(codepoint, glyph).first->second;
}


void FontLibrary::scanSystemFonts()
{
    std::vector<FontInfo> fonts;

    for (const std::filesystem::path& directory : systemFontDirectories()) {
        std::error_code error;

        for (auto it = std::filesystem::recursive_directory_iterator(directory, std::filesystem::directory_options::skip_permission_denied, error);
             !error && it != std::filesystem::recursive_directory_iterator(); it.increment(error)) {
            if (!it->is_regular_file(error)) {
                continue;
            }

            std::string extension = it->path().extension().string();
            std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) { return (char)std::tolower(c); });

            if (extension != ".ttf" && extension != ".otf" && extension != ".ttc") {
                continue;
            }

            FontInfo info;

            if (readFontNames(it->path(), info.family, info.style)) {
                const std::u8string u8 = it->path().u8string();
                info.path.assign(u8.begin(), u8.end());
                fonts.push_back(std::move(info));
            }
        }
    }

    std::sort(fonts.begin(), fonts.end(), [](const FontInfo& a, const FontInfo& b) {
        return a.family != b.family ? a.family < b.family : a.style < b.style;
    });

    // The same font may be installed for the system and for the user.
    fonts.erase(std::unique(fonts.begin(), fonts.end(), [](const FontInfo& a, const FontInfo& b) {
        return a.family == b.family && a.style == b.style;
    }), fonts.end());

    std::scoped_lock lock(_systemFontsMutex);
    _systemFonts = std::move(fonts);
}


std::vector<FontLibrary::FontInfo> FontLibrary::systemFonts() const
{
    std::scoped_lock lock(_systemFontsMutex);
    return _systemFonts;
}


std::string FontLibrary::variant(const std::string& path, const std::string& style) const
{
    std::scoped_lock lock(_systemFontsMutex);

    const auto current = std::find_if(_systemFonts.begin(), _systemFonts.end(), [&](const FontInfo& f) { return f.path == path; });
    const std::string family = (current != _systemFonts.end()) ? current->family : std::string();

    for (const FontInfo& f : _systemFonts) {
        if (f.family == family && f.style == style) {
            return f.path;
        }
    }

    return {};
}


std::string FontLibrary::styleOf(const std::string& path) const
{
    std::scoped_lock lock(_systemFontsMutex);

    for (const FontInfo& f : _systemFonts) {
        if (f.path == path) {
            return f.style;
        }
    }

    return "Regular";
}

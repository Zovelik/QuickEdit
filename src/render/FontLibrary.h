#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>


// Fonts of the texts: the default one (Inter, embedded) and the fonts of
// the system, with their glyphs rendered as signed distance fields in an
// atlas the GPU samples. A distance field stays sharp at any size and gives
// outlines and soft shadows for free in the shader.
//
// Main thread only, except the scan of the system fonts which runs in the
// background.
class FontLibrary
{
public:
    // Glyphs are rendered at this size (pixels per em), with this margin of
    // distance around them (pixels at that size).
    static constexpr float BaseSize = 64.f;
    static constexpr int Padding = 12;

    static constexpr uint32_t AtlasSize = 2048;

    // A font file of the system.
    struct FontInfo
    {
        std::string path;           // UTF-8
        std::string family;
        std::string style;          // Regular, Bold, Italic...

        std::string displayName() const { return family + (style.empty() || style == "Regular" ? "" : " " + style); }
    };

    struct Glyph
    {
        // Rectangle in the atlas, in texels, and its placement relative to
        // the pen position at BaseSize (y down), padding included.
        uint16_t x = 0, y = 0, width = 0, height = 0;
        float offsetX = 0.f, offsetY = 0.f;
        float advance = 0.f;        // At BaseSize
        bool empty = true;          // Spaces...
    };

    // Vertical metrics at BaseSize.
    struct Metrics
    {
        float ascent = 0.f;
        float descent = 0.f;        // Negative
        float lineGap = 0.f;
    };

    // defaultFont: TTF data of the default font. scanSystemFonts: list the
    // fonts of the system in the background, for choosing them (the fonts
    // used by path load without it).
    explicit FontLibrary(std::vector<uint8_t> defaultFont, bool scanSystemFonts = true);
    ~FontLibrary();

    // Index of the font of the given file (empty: the default font), loaded
    // on first use. The default font when the file cannot be read.
    int font(const std::string& path);

    const Metrics& metrics(int font) const;

    // Glyph of a code point, rendered into the atlas on first use. A missing
    // glyph comes from the default font.
    const Glyph& glyph(int font, uint32_t codepoint);

    // Extra advance between two code points (kerning), at BaseSize.
    float kerning(int font, uint32_t left, uint32_t right);

    // The atlas, one byte per texel: distance 0 at 128, +16 per pixel
    // inside. dirty() until markUploaded().
    const std::vector<uint8_t>& atlas() const { return _atlas; }
    bool dirty() const { return _dirty; }
    void markUploaded() { _dirty = false; }

    // Incremented when the atlas was cleared to make room: glyphs from
    // before are no longer valid.
    uint64_t generation() const { return _generation; }

    // Fonts of the system, sorted by name. Empty until the background scan
    // is done.
    std::vector<FontInfo> systemFonts() const;

    // The font of the same family as path in the given style ("Bold"...),
    // or an empty string.
    std::string variant(const std::string& path, const std::string& style) const;

    // Style name of a system font ("Regular" for the default font).
    std::string styleOf(const std::string& path) const;

private:
    struct Font;

    void scanSystemFonts();
    bool allocate(uint16_t width, uint16_t height, uint16_t& x, uint16_t& y);
    void clearAtlas();

    std::vector<std::unique_ptr<Font>> _fonts;
    std::map<std::string, int> _fontByPath;

    std::vector<uint8_t> _atlas;
    bool _dirty = true;
    uint64_t _generation = 0;

    // Shelf packing of the atlas.
    uint16_t _shelfX = 0;
    uint16_t _shelfY = 0;
    uint16_t _shelfHeight = 0;

    mutable std::mutex _systemFontsMutex;
    std::vector<FontInfo> _systemFonts;
    std::thread _scanThread;
};

#include <render/TextLayout.h>

#include <algorithm>
#include <cstdint>


using namespace core;


namespace {

// A character of the text with what lays it out.
struct Item
{
    uint32_t codepoint;
    int span;
    int font;
    float scale;        // Style size / BaseSize
    float advance;      // Pixels, kerning and letter spacing included
    int letter;
};


struct Line
{
    size_t first = 0;   // Items [first, last)
    size_t last = 0;
    float width = 0.f;
    float ascent = 0.f;
    float descent = 0.f;
    float size = 0.f;   // Largest style size
};

} // namespace


TextLayout layoutText(FontLibrary& fonts, const TextContent& text)
{
    TextLayout layout;

    // Atlas cleared while rendering the glyphs (full): lay out again, all
    // the glyphs are then rendered in the new atlas.
    for (int attempt = 0; attempt < 2; attempt++) {
        const uint64_t generation = fonts.generation();

        std::vector<Item> items;
        int letter = 0;

        for (int s = 0; s < (int)text.spans.size(); s++) {
            const TextSpan& span = text.spans[s];
            const int font = fonts.font(span.style.font);
            const float scale = span.style.size / FontLibrary::BaseSize;

            for (size_t i = 0; i < span.text.size();) {
                const uint32_t codepoint = nextCodepoint(span.text, i);
                Item item = { codepoint, s, font, scale, 0.f, -1 };

                if (codepoint != '\n') {
                    item.advance = fonts.glyph(font, codepoint).advance * scale + span.style.letterSpacing;
                    item.letter = letter++;

                    // Kerning with the previous character of the same font.
                    if (!items.empty() && items.back().font == font && items.back().codepoint != '\n') {
                        items.back().advance += fonts.kerning(font, items.back().codepoint, codepoint) * scale;
                    }
                }

                items.push_back(item);
            }
        }

        layout.letterCount = (size_t)letter;

        // Lines: explicit breaks, and wrapping at the last space before the
        // box width.
        std::vector<Line> lines;
        Line line;
        size_t lastSpace = SIZE_MAX;

        for (size_t i = 0; i < items.size(); i++) {
            const Item& item = items[i];

            if (item.codepoint == '\n') {
                line.last = i;
                lines.push_back(line);
                line = Line();
                line.first = i + 1;
                lastSpace = SIZE_MAX;
                continue;
            }

            if (text.boxWidth > 0.f && line.width + item.advance > text.boxWidth && lastSpace != SIZE_MAX && lastSpace > line.first) {
                // The space ends the line and is not shown.
                Line wrapped = line;
                wrapped.last = lastSpace;
                lines.push_back(wrapped);

                line = Line();
                line.first = lastSpace + 1;
                lastSpace = SIZE_MAX;

                for (size_t k = line.first; k < i; k++) {
                    line.width += items[k].advance;
                }
            }

            if (item.codepoint == ' ') {
                lastSpace = i;
            }

            line.width += item.advance;
        }

        line.last = items.size();
        lines.push_back(line);

        // Vertical metrics of the lines, and widths without the trailing
        // spaces.
        float blockWidth = text.boxWidth;
        const TextStyle defaultStyle;

        for (Line& l : lines) {
            l.width = 0.f;

            for (size_t i = l.first; i < l.last; i++) {
                const Item& item = items[i];
                const FontLibrary::Metrics& metrics = fonts.metrics(item.font);

                l.ascent = std::max(l.ascent, metrics.ascent * item.scale);
                l.descent = std::min(l.descent, metrics.descent * item.scale);
                l.size = std::max(l.size, item.scale * FontLibrary::BaseSize);
            }

            size_t end = l.last;

            while (end > l.first && items[end - 1].codepoint == ' ') {
                end--;
            }

            for (size_t i = l.first; i < end; i++) {
                l.width += items[i].advance;
            }

            // An empty line keeps the height of the default style.
            if (l.size == 0.f) {
                const float scale = (text.spans.empty() ? defaultStyle.size : text.spans.back().style.size) / FontLibrary::BaseSize;
                l.ascent = fonts.metrics(0).ascent * scale;
                l.descent = fonts.metrics(0).descent * scale;
                l.size = scale * FontLibrary::BaseSize;
            }

            blockWidth = std::max(blockWidth, l.width);
        }

        // Baselines
        std::vector<float> baselines(lines.size());
        float y = 0.f;

        for (size_t i = 0; i < lines.size(); i++) {
            // Natural distance: the descent of the line above, the ascent of
            // this one; the line spacing scales it.
            baselines[i] = (i == 0) ? lines[i].ascent : baselines[i - 1] + (lines[i].ascent - lines[i - 1].descent) * text.lineSpacing;
        }

        y = baselines.back() - lines.back().descent;

        layout.width = blockWidth;
        layout.height = y;
        layout.glyphs.clear();

        const float invAtlas = 1.f / (float)FontLibrary::AtlasSize;

        layout.characters.assign(items.size(), CharacterBox{ 0.f, 0.f, 0.f, 0.f, 0 });
        layout.lineCount = (int)lines.size();

        for (size_t l = 0; l < lines.size(); l++) {
            const Line& line = lines[l];

            float x = 0.f;

            switch (text.align) {
                case TextAlign::Center: x = 0.5f * (blockWidth - line.width); break;
                case TextAlign::Right:  x = blockWidth - line.width; break;
                default:                break;
            }

            const float top = baselines[l] - line.ascent - 0.5f * layout.height;
            const float bottom = baselines[l] - line.descent - 0.5f * layout.height;

            for (size_t i = line.first; i < line.last; i++) {
                const Item& item = items[i];
                const FontLibrary::Glyph& glyph = fonts.glyph(item.font, item.codepoint);

                layout.characters[i] = { x - 0.5f * blockWidth, x + item.advance - 0.5f * blockWidth, top, bottom, (int)l };

                if (!glyph.empty) {
                    LaidOutGlyph out;
                    out.x0 = x + glyph.offsetX * item.scale - 0.5f * blockWidth;
                    out.y0 = baselines[l] + glyph.offsetY * item.scale - 0.5f * layout.height;
                    out.x1 = out.x0 + glyph.width * item.scale;
                    out.y1 = out.y0 + glyph.height * item.scale;
                    out.u0 = glyph.x * invAtlas;
                    out.v0 = glyph.y * invAtlas;
                    out.u1 = (glyph.x + glyph.width) * invAtlas;
                    out.v1 = (glyph.y + glyph.height) * invAtlas;
                    out.pixelsPerBase = item.scale;
                    out.span = item.span;
                    out.letter = item.letter;
                    out.pivotX = x + 0.5f * item.advance - 0.5f * blockWidth;
                    out.pivotY = baselines[l] - 0.35f * line.size - 0.5f * layout.height;
                    layout.glyphs.push_back(out);
                }

                x += item.advance;
            }

            // The line break (or the space a wrap removed) ending the line.
            if (line.last < items.size()) {
                layout.characters[line.last] = { x - 0.5f * blockWidth, x - 0.5f * blockWidth, top, bottom, (int)l };
            }
        }

        if (fonts.generation() == generation) {
            break;
        }
    }

    return layout;
}

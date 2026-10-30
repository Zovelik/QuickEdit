#include <core/TextEdit.h>

#include <algorithm>
#include <cstddef>
#include <vector>


namespace core
{

namespace {

struct Character
{
    uint32_t codepoint;
    int style;          // Index in the styles
};


void appendUtf8(std::string& out, uint32_t codepoint)
{
    if (codepoint < 0x80) {
        out += (char)codepoint;
    }
    else if (codepoint < 0x800) {
        out += (char)(0xC0 | (codepoint >> 6));
        out += (char)(0x80 | (codepoint & 0x3F));
    }
    else if (codepoint < 0x10000) {
        out += (char)(0xE0 | (codepoint >> 12));
        out += (char)(0x80 | ((codepoint >> 6) & 0x3F));
        out += (char)(0x80 | (codepoint & 0x3F));
    }
    else {
        out += (char)(0xF0 | (codepoint >> 18));
        out += (char)(0x80 | ((codepoint >> 12) & 0x3F));
        out += (char)(0x80 | ((codepoint >> 6) & 0x3F));
        out += (char)(0x80 | (codepoint & 0x3F));
    }
}


// The text as characters, each with the index of its style.
void explode(const TextContent& text, std::vector<Character>& characters, std::vector<TextStyle>& styles)
{
    characters.clear();
    styles.clear();

    for (const TextSpan& span : text.spans) {
        styles.push_back(span.style);
        const int style = (int)styles.size() - 1;

        for (size_t i = 0; i < span.text.size();) {
            characters.push_back({ nextCodepoint(span.text, i), style });
        }
    }

    if (styles.empty()) {
        styles.push_back(TextStyle());
    }
}


// Spans of the characters, consecutive characters of equal styles merged.
// An empty text keeps one empty span, with the style typing would use.
void rebuild(TextContent& text, const std::vector<Character>& characters, const std::vector<TextStyle>& styles, int emptyStyle)
{
    text.spans.clear();

    for (const Character& character : characters) {
        const TextStyle& style = styles[character.style];

        if (text.spans.empty() || !(text.spans.back().style == style)) {
            text.spans.push_back({ {}, style });
        }

        appendUtf8(text.spans.back().text, character.codepoint);
    }

    if (text.spans.empty()) {
        text.spans.push_back({ {}, styles[std::clamp(emptyStyle, 0, (int)styles.size() - 1)] });
    }
}


int styleIndexAt(const std::vector<Character>& characters, size_t pos)
{
    if (characters.empty()) {
        return 0;
    }

    return characters[std::min(pos > 0 ? pos - 1 : 0, characters.size() - 1)].style;
}


bool isWordCharacter(uint32_t codepoint)
{
    return codepoint != ' ' && codepoint != '\n' && codepoint != '\t' && codepoint != ',' && codepoint != '.' &&
           codepoint != ';' && codepoint != ':' && codepoint != '!' && codepoint != '?';
}

} // namespace


size_t textLength(const TextContent& text)
{
    size_t length = 0;

    for (const TextSpan& span : text.spans) {
        for (size_t i = 0; i < span.text.size();) {
            nextCodepoint(span.text, i);
            length++;
        }
    }

    return length;
}


TextStyle styleAt(const TextContent& text, size_t pos)
{
    std::vector<Character> characters;
    std::vector<TextStyle> styles;
    explode(text, characters, styles);

    return styles[styleIndexAt(characters, pos)];
}


size_t insertText(TextContent& text, size_t pos, const std::string& utf8)
{
    std::vector<Character> characters;
    std::vector<TextStyle> styles;
    explode(text, characters, styles);

    pos = std::min(pos, characters.size());
    const int style = styleIndexAt(characters, pos);

    std::vector<Character> inserted;

    for (size_t i = 0; i < utf8.size();) {
        const uint32_t codepoint = nextCodepoint(utf8, i);

        // Line breaks only as '\n'.
        if (codepoint != '\r') {
            inserted.push_back({ codepoint, style });
        }
    }

    characters.insert(characters.begin() + (std::ptrdiff_t)pos, inserted.begin(), inserted.end());
    rebuild(text, characters, styles, style);

    return pos + inserted.size();
}


void eraseText(TextContent& text, size_t from, size_t to)
{
    std::vector<Character> characters;
    std::vector<TextStyle> styles;
    explode(text, characters, styles);

    from = std::min(from, characters.size());
    to = std::clamp(to, from, characters.size());

    const int style = styleIndexAt(characters, from + 1);
    characters.erase(characters.begin() + (std::ptrdiff_t)from, characters.begin() + (std::ptrdiff_t)to);
    rebuild(text, characters, styles, style);
}


std::string textRange(const TextContent& text, size_t from, size_t to)
{
    std::vector<Character> characters;
    std::vector<TextStyle> styles;
    explode(text, characters, styles);

    std::string out;

    for (size_t i = from; i < std::min(to, characters.size()); i++) {
        appendUtf8(out, characters[i].codepoint);
    }

    return out;
}


void applyStyleChange(TextContent& text, size_t from, size_t to, const TextStyle& before, const TextStyle& after)
{
    std::vector<Character> characters;
    std::vector<TextStyle> styles;
    explode(text, characters, styles);

    from = std::min(from, characters.size());
    to = std::clamp(to, from, characters.size());

    // Without a selection, the whole text.
    if (from == to) {
        from = 0;
        to = characters.size();
    }

    auto changed = [&](const TextStyle& style) {
        TextStyle result = style;

#define APPLY_FIELD(field) if (!(before.field == after.field)) result.field = after.field;
        APPLY_FIELD(font)
        APPLY_FIELD(size)
        APPLY_FIELD(color)
        APPLY_FIELD(letterSpacing)
        APPLY_FIELD(outlineWidth)
        APPLY_FIELD(outlineColor)
        APPLY_FIELD(shadowOffsetX)
        APPLY_FIELD(shadowOffsetY)
        APPLY_FIELD(shadowSoftness)
        APPLY_FIELD(shadowColor)
#undef APPLY_FIELD

        return result;
    };

    for (size_t i = from; i < to; i++) {
        const TextStyle style = changed(styles[characters[i].style]);
        const auto it = std::find(styles.begin(), styles.end(), style);

        if (it != styles.end()) {
            characters[i].style = (int)(it - styles.begin());
        }
        else {
            styles.push_back(style);
            characters[i].style = (int)styles.size() - 1;
        }
    }

    // An empty text: its only style changes.
    if (characters.empty()) {
        styles[0] = changed(styles[0]);
    }

    rebuild(text, characters, styles, 0);
}


size_t wordStart(const TextContent& text, size_t pos)
{
    std::vector<Character> characters;
    std::vector<TextStyle> styles;
    explode(text, characters, styles);

    pos = std::min(pos, characters.size());

    while (pos > 0 && isWordCharacter(characters[pos - 1].codepoint)) {
        pos--;
    }

    return pos;
}


size_t wordEnd(const TextContent& text, size_t pos)
{
    std::vector<Character> characters;
    std::vector<TextStyle> styles;
    explode(text, characters, styles);

    while (pos < characters.size() && isWordCharacter(characters[pos].codepoint)) {
        pos++;
    }

    return pos;
}

}

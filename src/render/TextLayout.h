#pragma once

#include <render/FontLibrary.h>

#include <core/Text.h>

#include <vector>


// A glyph placed in a text, in pixels of the sequence, y down, relative to
// the center of the text block.
struct LaidOutGlyph
{
    float x0, y0, x1, y1;           // Quad, padding of the distance field included
    float u0, v0, u1, v1;           // In the atlas
    float pixelsPerBase;            // Pixels of the sequence per pixel of the distance field
    int span;                       // Index of the span, for its style
    int letter;                     // Index of the letter in the text, for the animations

    // Center of the letter on its line, around which it scales and rises.
    float pivotX, pivotY;
};


// Box of a character, in the same coordinates as the glyphs, for the caret
// and the selection of the text editor.
struct CharacterBox
{
    float x0, x1;                   // Pen position before and after the character
    float top, bottom;              // Of its line
    int line;
};


struct TextLayout
{
    std::vector<LaidOutGlyph> glyphs;

    // One per character of the text (line breaks included), in order.
    std::vector<CharacterBox> characters;
    int lineCount = 0;

    float width = 0.f;
    float height = 0.f;
    size_t letterCount = 0;
};


// Lays the text out: styled spans, line breaks, wrapping at the box width
// on spaces, alignment. Renders the missing glyphs into the atlas.
TextLayout layoutText(FontLibrary& fonts, const core::TextContent& text);

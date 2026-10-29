#pragma once

#include <core/Text.h>

#include <string>


namespace core
{
    // Editing of a styled text by character positions (code points, line
    // breaks included) across its spans, as a text box editor does: the
    // spans are cut and merged so that the text keeps its styles.

    // Count of characters.
    size_t textLength(const TextContent& text);

    // Style of the character before pos (the style typed text takes), the
    // first one at the start.
    TextStyle styleAt(const TextContent& text, size_t pos);

    // Inserts UTF-8 text at pos, in the style of the character before it.
    // Returns the position after the inserted text.
    size_t insertText(TextContent& text, size_t pos, const std::string& utf8);

    // Removes the characters [from, to).
    void eraseText(TextContent& text, size_t from, size_t to);

    // Characters [from, to), as UTF-8.
    std::string textRange(const TextContent& text, size_t from, size_t to);

    // Applies to the characters [from, to) the fields of after that differ
    // from before: a change of color keeps the sizes of a selection that
    // mixes sizes.
    void applyStyleChange(TextContent& text, size_t from, size_t to, const TextStyle& before, const TextStyle& after);

    // Position of the start of the word around pos, and of its end.
    size_t wordStart(const TextContent& text, size_t pos);
    size_t wordEnd(const TextContent& text, size_t pos);
}

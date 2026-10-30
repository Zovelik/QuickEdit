#pragma once

#include <core/Time.h>

#include <cstdint>

#include <string>
#include <vector>


namespace core
{
    struct Color
    {
        float r = 1.f;
        float g = 1.f;
        float b = 1.f;
        float a = 1.f;

        bool operator==(const Color& other) const = default;
    };


    // Look of a run of characters.
    struct TextStyle
    {
        // Font file; empty for the default font (Inter).
        std::string font;

        float size = 96.f;              // In pixels of the sequence
        Color color;
        float letterSpacing = 0.f;      // Extra pixels between letters

        float outlineWidth = 0.f;       // Pixels, 0 for none
        Color outlineColor = { 0.f, 0.f, 0.f, 1.f };

        float shadowOffsetX = 0.f;      // Pixels, both 0 for none
        float shadowOffsetY = 0.f;
        float shadowSoftness = 0.f;     // Pixels of blur
        Color shadowColor = { 0.f, 0.f, 0.f, 0.6f };

        bool operator==(const TextStyle& other) const = default;
    };


    // Characters sharing a style. A text is a sequence of spans, so that a
    // single word can be bold or of another color.
    struct TextSpan
    {
        std::string text;               // UTF-8, '\n' for line breaks
        TextStyle style;
    };


    enum class TextAlign
    {
        Left,
        Center,
        Right
    };


    // Animation of the letters when the text appears (and disappears).
    enum class LetterAnimation
    {
        None,
        Typewriter,     // Letters appear one by one
        Fade,           // Letters fade in one by one
        Rise,           // Letters fade in and rise from below
        Pop             // Letters fade in and grow from small
    };


    struct TextAnimation
    {
        LetterAnimation in = LetterAnimation::None;
        LetterAnimation out = LetterAnimation::None;

        // Delay between the starts of two letters, and length of the
        // animation of one letter.
        Time letterDelay = TicksPerSecond / 30;
        Time letterDuration = TicksPerSecond / 4;
    };


    // Movement of a whole text over its clip.
    enum class TextMotion
    {
        None,
        Roll,       // From below the frame to above it (end credits)
        Crawl       // From the right of the frame to its left (news ticker)
    };


    // A box behind a text.
    struct TextBackground
    {
        bool enabled = false;
        Color color = { 0.f, 0.f, 0.f, 0.65f };
        float padding = 24.f;           // Pixels around the text
        float cornerRadius = 8.f;
        bool fullWidth = false;         // Across the whole frame (a bar)

        bool operator==(const TextBackground& other) const = default;
    };


    struct TextContent
    {
        std::vector<TextSpan> spans;
        TextAlign align = TextAlign::Center;
        float lineSpacing = 1.f;        // Times the natural line height of the fonts

        // Lines wrap at this width (pixels), 0 for no wrapping.
        float boxWidth = 0.f;

        TextAnimation animation;

        TextMotion motion = TextMotion::None;
        TextBackground background;

        // Text without the styles.
        std::string plainText() const;

        // Count of characters (code points), line breaks excluded.
        size_t letterCount() const;
    };


    // Decodes the UTF-8 code point at text[i] and advances i. Invalid bytes
    // decode as U+FFFD.
    uint32_t nextCodepoint(const std::string& text, size_t& i);
}

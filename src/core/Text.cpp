#include <core/Text.h>


namespace core
{

uint32_t nextCodepoint(const std::string& text, size_t& i)
{
    const unsigned char c = (unsigned char)text[i++];

    if (c < 0x80) {
        return c;
    }

    int extra = 0;
    uint32_t codepoint = 0;

    if ((c & 0xE0) == 0xC0)      { extra = 1; codepoint = c & 0x1F; }
    else if ((c & 0xF0) == 0xE0) { extra = 2; codepoint = c & 0x0F; }
    else if ((c & 0xF8) == 0xF0) { extra = 3; codepoint = c & 0x07; }
    else                         { return 0xFFFD; }

    for (int k = 0; k < extra; k++) {
        if (i >= text.size() || ((unsigned char)text[i] & 0xC0) != 0x80) {
            return 0xFFFD;
        }

        codepoint = (codepoint << 6) | ((unsigned char)text[i++] & 0x3F);
    }

    return codepoint;
}


std::string TextContent::plainText() const
{
    std::string text;

    for (const TextSpan& span : spans) {
        text += span.text;
    }

    return text;
}


size_t TextContent::letterCount() const
{
    size_t count = 0;

    for (const TextSpan& span : spans) {
        for (size_t i = 0; i < span.text.size();) {
            if (nextCodepoint(span.text, i) != '\n') {
                count++;
            }
        }
    }

    return count;
}

}

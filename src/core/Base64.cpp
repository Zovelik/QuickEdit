#include <core/Base64.h>


namespace core
{

namespace {

const char Alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";


int decodeCharacter(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

} // namespace


std::string base64Encode(const std::vector<uint8_t>& data)
{
    std::string out;
    out.reserve((data.size() + 2) / 3 * 4);

    for (size_t i = 0; i < data.size(); i += 3) {
        const uint32_t a = data[i];
        const uint32_t b = (i + 1 < data.size()) ? data[i + 1] : 0;
        const uint32_t c = (i + 2 < data.size()) ? data[i + 2] : 0;
        const uint32_t triple = (a << 16) | (b << 8) | c;

        out += Alphabet[(triple >> 18) & 63];
        out += Alphabet[(triple >> 12) & 63];
        out += (i + 1 < data.size()) ? Alphabet[(triple >> 6) & 63] : '=';
        out += (i + 2 < data.size()) ? Alphabet[triple & 63] : '=';
    }

    return out;
}


std::vector<uint8_t> base64Decode(const std::string& text)
{
    std::vector<uint8_t> out;
    out.reserve(text.size() / 4 * 3);

    uint32_t buffer = 0;
    int bits = 0;

    for (const char c : text) {
        const int value = decodeCharacter(c);

        if (value < 0) {
            continue;
        }

        buffer = (buffer << 6) | (uint32_t)value;
        bits += 6;

        if (bits >= 8) {
            bits -= 8;
            out.push_back((uint8_t)((buffer >> bits) & 0xFF));
        }
    }

    return out;
}

}

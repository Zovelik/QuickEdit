#include <core/Json.h>

#include <charconv>
#include <cmath>
#include <cstdio>
#include <stdexcept>


namespace core
{

namespace {

const Json NullJson;
const Json::Array EmptyArray;
const Json::Object EmptyObject;


void appendEscaped(std::string& out, const std::string& value)
{
    out += '"';

    for (const unsigned char c : value) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (c < 0x20) {
                    char buffer[8];
                    std::snprintf(buffer, sizeof(buffer), "\\u%04x", c);
                    out += buffer;
                }
                else {
                    // UTF-8 bytes are kept as they are.
                    out += (char)c;
                }
                break;
        }
    }

    out += '"';
}


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


// Recursive descent parser over the whole text.
class Parser
{
public:
    explicit Parser(const std::string& text) : _text(text) {}

    Json parseDocument()
    {
        Json value = parseValue();
        skipWhitespace();

        if (_pos != _text.size()) {
            fail("unexpected characters after the document");
        }

        return value;
    }

private:
    [[noreturn]] void fail(const char* message) const
    {
        throw std::runtime_error(std::string("JSON: ") + message + " at offset " + std::to_string(_pos));
    }

    void skipWhitespace()
    {
        while (_pos < _text.size() && (_text[_pos] == ' ' || _text[_pos] == '\t' || _text[_pos] == '\n' || _text[_pos] == '\r')) {
            _pos++;
        }
    }

    bool consume(char c)
    {
        skipWhitespace();

        if (_pos < _text.size() && _text[_pos] == c) {
            _pos++;
            return true;
        }

        return false;
    }

    void expect(char c)
    {
        if (!consume(c)) {
            fail((std::string("expected '") + c + "'").c_str());
        }
    }

    bool consumeLiteral(const char* literal)
    {
        const size_t length = std::char_traits<char>::length(literal);

        if (_text.compare(_pos, length, literal) == 0) {
            _pos += length;
            return true;
        }

        return false;
    }

    Json parseValue()
    {
        if (++_depth > 256) {
            fail("document too deep");
        }

        skipWhitespace();

        if (_pos >= _text.size()) {
            fail("unexpected end of document");
        }

        Json value;
        const char c = _text[_pos];

        if (c == '{') {
            value = parseObject();
        }
        else if (c == '[') {
            value = parseArray();
        }
        else if (c == '"') {
            value = Json(parseString());
        }
        else if (consumeLiteral("true")) {
            value = Json(true);
        }
        else if (consumeLiteral("false")) {
            value = Json(false);
        }
        else if (consumeLiteral("null")) {
            value = Json();
        }
        else {
            value = parseNumber();
        }

        _depth--;

        return value;
    }

    Json parseObject()
    {
        expect('{');
        Json value = Json::object();

        if (consume('}')) {
            return value;
        }

        do {
            skipWhitespace();

            if (_pos >= _text.size() || _text[_pos] != '"') {
                fail("expected a key");
            }

            const std::string key = parseString();
            expect(':');
            value[key] = parseValue();
        } while (consume(','));

        expect('}');

        return value;
    }

    Json parseArray()
    {
        expect('[');
        Json value = Json::array();

        if (consume(']')) {
            return value;
        }

        do {
            value.push_back(parseValue());
        } while (consume(','));

        expect(']');

        return value;
    }

    uint32_t parseHex4()
    {
        if (_pos + 4 > _text.size()) {
            fail("truncated escape sequence");
        }

        uint32_t value = 0;
        const auto result = std::from_chars(_text.data() + _pos, _text.data() + _pos + 4, value, 16);

        if (result.ptr != _text.data() + _pos + 4) {
            fail("invalid escape sequence");
        }

        _pos += 4;

        return value;
    }

    std::string parseString()
    {
        // Opening quote, already checked by the caller.
        _pos++;
        std::string value;

        while (true) {
            if (_pos >= _text.size()) {
                fail("unterminated string");
            }

            const char c = _text[_pos++];

            if (c == '"') {
                break;
            }

            if (c != '\\') {
                value += c;
                continue;
            }

            if (_pos >= _text.size()) {
                fail("unterminated string");
            }

            switch (_text[_pos++]) {
                case '"':  value += '"';  break;
                case '\\': value += '\\'; break;
                case '/':  value += '/';  break;
                case 'b':  value += '\b'; break;
                case 'f':  value += '\f'; break;
                case 'n':  value += '\n'; break;
                case 'r':  value += '\r'; break;
                case 't':  value += '\t'; break;
                case 'u': {
                    uint32_t codepoint = parseHex4();

                    // Surrogate pair
                    if (codepoint >= 0xD800 && codepoint < 0xDC00 && consumeLiteral("\\u")) {
                        const uint32_t low = parseHex4();
                        codepoint = 0x10000 + ((codepoint - 0xD800) << 10) + (low - 0xDC00);
                    }

                    appendUtf8(value, codepoint);
                    break;
                }
                default:
                    fail("invalid escape sequence");
            }
        }

        return value;
    }

    Json parseNumber()
    {
        const size_t start = _pos;
        bool isInteger = true;

        if (_pos < _text.size() && _text[_pos] == '-') {
            _pos++;
        }

        while (_pos < _text.size()) {
            const char c = _text[_pos];

            if (c >= '0' && c <= '9') {
                _pos++;
            }
            else if (c == '.' || c == 'e' || c == 'E' || c == '+' || c == '-') {
                isInteger = false;
                _pos++;
            }
            else {
                break;
            }
        }

        if (_pos == start) {
            fail("unexpected character");
        }

        const char* first = _text.data() + start;
        const char* last  = _text.data() + _pos;

        if (isInteger) {
            int64_t value = 0;

            if (std::from_chars(first, last, value).ptr == last) {
                return Json(value);
            }
        }

        double value = 0.0;

        if (std::from_chars(first, last, value).ptr != last) {
            fail("invalid number");
        }

        return Json(value);
    }

    const std::string& _text;
    size_t _pos = 0;
    int _depth = 0;
};

} // namespace


bool Json::toBool(bool defaultValue) const
{
    if (const bool* value = std::get_if<bool>(&_value)) {
        return *value;
    }

    return defaultValue;
}


int64_t Json::toInt(int64_t defaultValue) const
{
    if (const int64_t* value = std::get_if<int64_t>(&_value)) {
        return *value;
    }

    if (const double* value = std::get_if<double>(&_value)) {
        return (int64_t)std::llround(*value);
    }

    return defaultValue;
}


double Json::toDouble(double defaultValue) const
{
    if (const double* value = std::get_if<double>(&_value)) {
        return *value;
    }

    if (const int64_t* value = std::get_if<int64_t>(&_value)) {
        return (double)*value;
    }

    return defaultValue;
}


std::string Json::toString(const std::string& defaultValue) const
{
    if (const std::string* value = std::get_if<std::string>(&_value)) {
        return *value;
    }

    return defaultValue;
}


size_t Json::size() const
{
    if (isArray()) {
        return items().size();
    }

    if (isObject()) {
        return members().size();
    }

    return 0;
}


const Json& Json::operator[](size_t index) const
{
    const Array& array = items();

    return index < array.size() ? array[index] : NullJson;
}


void Json::push_back(const Json& value)
{
    mutableArray().push_back(value);
}


Json& Json::operator[](const std::string& key)
{
    return mutableObject()[key];
}


const Json& Json::operator[](const std::string& key) const
{
    const Object& object = members();
    const auto it = object.find(key);

    return it != object.end() ? it->second : NullJson;
}


bool Json::contains(const std::string& key) const
{
    return members().count(key) != 0;
}


const Json::Array& Json::items() const
{
    if (const auto* value = std::get_if<std::shared_ptr<Array>>(&_value)) {
        return **value;
    }

    return EmptyArray;
}


const Json::Object& Json::members() const
{
    if (const auto* value = std::get_if<std::shared_ptr<Object>>(&_value)) {
        return **value;
    }

    return EmptyObject;
}


Json::Array& Json::mutableArray()
{
    if (!isArray()) {
        _value = std::make_shared<Array>();
    }

    auto& array = std::get<std::shared_ptr<Array>>(_value);

    if (array.use_count() > 1) {
        array = std::make_shared<Array>(*array);
    }

    return *array;
}


Json::Object& Json::mutableObject()
{
    if (!isObject()) {
        _value = std::make_shared<Object>();
    }

    auto& object = std::get<std::shared_ptr<Object>>(_value);

    if (object.use_count() > 1) {
        object = std::make_shared<Object>(*object);
    }

    return *object;
}


std::string Json::dump(int indent) const
{
    std::string out;
    dump(out, indent, 0);
    out += '\n';

    return out;
}


void Json::dump(std::string& out, int indent, int depth) const
{
    auto newline = [&](int level) {
        if (indent > 0) {
            out += '\n';
            out.append((size_t)(indent * level), ' ');
        }
    };

    if (isNull()) {
        out += "null";
    }
    else if (const bool* value = std::get_if<bool>(&_value)) {
        out += *value ? "true" : "false";
    }
    else if (const int64_t* value = std::get_if<int64_t>(&_value)) {
        out += std::to_string(*value);
    }
    else if (const double* value = std::get_if<double>(&_value)) {
        if (!std::isfinite(*value)) {
            out += "null";
        }
        else {
            // Shortest representation that reads back to the same value.
            char buffer[32];
            const auto result = std::to_chars(buffer, buffer + sizeof(buffer), *value);
            out.append(buffer, result.ptr);
        }
    }
    else if (const std::string* value = std::get_if<std::string>(&_value)) {
        appendEscaped(out, *value);
    }
    else if (isArray()) {
        const Array& array = items();

        if (array.empty()) {
            out += "[]";
            return;
        }

        out += '[';

        for (size_t i = 0; i < array.size(); i++) {
            if (i > 0) {
                out += ',';
            }

            newline(depth + 1);
            array[i].dump(out, indent, depth + 1);
        }

        newline(depth);
        out += ']';
    }
    else {
        const Object& object = members();

        if (object.empty()) {
            out += "{}";
            return;
        }

        out += '{';
        bool first = true;

        for (const auto& [key, value] : object) {
            if (!first) {
                out += ',';
            }

            first = false;
            newline(depth + 1);
            appendEscaped(out, key);
            out += indent > 0 ? ": " : ":";
            value.dump(out, indent, depth + 1);
        }

        newline(depth);
        out += '}';
    }
}


Json Json::parse(const std::string& text)
{
    return Parser(text).parseDocument();
}

}

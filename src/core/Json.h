#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <variant>
#include <vector>


namespace core
{
    // Minimal JSON document, enough for the project files: objects keep
    // their keys sorted, numbers are stored as int64 when they have no
    // fractional part so that times (in ticks) round-trip exactly.
    class Json
    {
    public:
        using Array  = std::vector<Json>;
        using Object = std::map<std::string, Json>;

        Json() = default;
        Json(std::nullptr_t) {}
        Json(bool value)               : _value(value) {}
        Json(int value)                : _value((int64_t)value) {}
        Json(int64_t value)            : _value(value) {}
        Json(uint32_t value)           : _value((int64_t)value) {}
        Json(uint64_t value)           : _value((int64_t)value) {}
        Json(double value)             : _value(value) {}
        Json(const char* value)        : _value(std::string(value)) {}
        Json(const std::string& value) : _value(value) {}
        Json(const Array& value)       : _value(std::make_shared<Array>(value)) {}
        Json(const Object& value)      : _value(std::make_shared<Object>(value)) {}

        static Json array()  { return Json(Array()); }
        static Json object() { return Json(Object()); }

        bool isNull() const   { return std::holds_alternative<std::monostate>(_value); }
        bool isBool() const   { return std::holds_alternative<bool>(_value); }
        bool isNumber() const { return std::holds_alternative<int64_t>(_value) || std::holds_alternative<double>(_value); }
        bool isString() const { return std::holds_alternative<std::string>(_value); }
        bool isArray() const  { return std::holds_alternative<std::shared_ptr<Array>>(_value); }
        bool isObject() const { return std::holds_alternative<std::shared_ptr<Object>>(_value); }

        // Accessors returning the given default when the value has another
        // type: a missing or malformed field never throws.
        bool toBool(bool defaultValue = false) const;
        int64_t toInt(int64_t defaultValue = 0) const;
        double toDouble(double defaultValue = 0.0) const;
        std::string toString(const std::string& defaultValue = {}) const;

        // Array access. push_back() turns a null value into an array.
        size_t size() const;
        const Json& operator[](size_t index) const;
        void push_back(const Json& value);

        // Read only access on a non-const value too: without it, json[0]
        // would be ambiguous with the object access below.
        const Json& operator[](size_t index) { return static_cast<const Json&>(*this)[index]; }

        // Object access. The non-const operator turns a null value into an
        // object; the const one returns a null value for missing keys.
        Json& operator[](const std::string& key);
        const Json& operator[](const std::string& key) const;
        bool contains(const std::string& key) const;

        const Array& items() const;
        const Object& members() const;

        // Serialization. Throws std::runtime_error on a syntax error.
        std::string dump(int indent = 2) const;
        static Json parse(const std::string& text);

    private:
        // Arrays and objects are shared on copy and copied on write, see
        // mutableArray() and mutableObject().
        Array& mutableArray();
        Object& mutableObject();

        void dump(std::string& out, int indent, int depth) const;

        std::variant<
            std::monostate,
            bool,
            int64_t,
            double,
            std::string,
            std::shared_ptr<Array>,
            std::shared_ptr<Object>
        > _value;
    };
}

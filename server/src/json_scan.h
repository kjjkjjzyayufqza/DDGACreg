#pragma once

// Minimal JSON object scanner for the flat request bodies the game builds.
// Nested objects and arrays are skipped. Only the root object's keys are read.

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

struct JsonField {
    bool present = false;
    bool isBool = false;
    bool isString = false;
    bool boolean = false;
    double number = 0;
    std::string text;
};

inline void JsonSkipWs(const std::string& s, size_t& i)
{
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n'))
        ++i;
}

inline bool JsonParseString(const std::string& s, size_t& i, std::string& out)
{
    if (i >= s.size() || s[i] != '"')
        return false;
    ++i;
    out.clear();
    while (i < s.size()) {
        const char c = s[i++];
        if (c == '"')
            return true;
        if (c != '\\' || i >= s.size()) {
            out.push_back(c);
            continue;
        }
        const char e = s[i++];
        switch (e) {
        case '"':
        case '\\':
        case '/':
            out.push_back(e);
            break;
        case 'b':
            out.push_back('\b');
            break;
        case 'f':
            out.push_back('\f');
            break;
        case 'n':
            out.push_back('\n');
            break;
        case 'r':
            out.push_back('\r');
            break;
        case 't':
            out.push_back('\t');
            break;
        case 'u':
            if (i + 4 <= s.size()) {
                int cp = 0;
                for (int k = 0; k < 4; ++k) {
                    const char h = s[i++];
                    cp <<= 4;
                    if (h >= '0' && h <= '9')
                        cp += h - '0';
                    else if (h >= 'a' && h <= 'f')
                        cp += h - 'a' + 10;
                    else if (h >= 'A' && h <= 'F')
                        cp += h - 'A' + 10;
                }
                if (cp < 0x80) {
                    out.push_back(static_cast<char>(cp));
                } else if (cp < 0x800) {
                    out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
                    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                } else {
                    out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
                    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                }
            }
            break;
        default:
            out.push_back(e);
            break;
        }
    }
    return false;
}

inline void JsonSkipValue(const std::string& s, size_t& i)
{
    JsonSkipWs(s, i);
    if (i >= s.size())
        return;
    if (s[i] == '"') {
        std::string ignored;
        JsonParseString(s, i, ignored);
        return;
    }
    if (s[i] == '{' || s[i] == '[') {
        const char open = s[i];
        const char close = open == '{' ? '}' : ']';
        int depth = 0;
        bool inString = false;
        for (; i < s.size(); ++i) {
            const char c = s[i];
            if (inString) {
                if (c == '\\' && i + 1 < s.size()) {
                    ++i;
                    continue;
                }
                if (c == '"')
                    inString = false;
                continue;
            }
            if (c == '"') {
                inString = true;
                continue;
            }
            if (c == open)
                ++depth;
            else if (c == close) {
                --depth;
                if (depth == 0) {
                    ++i;
                    return;
                }
            }
        }
        return;
    }
    while (i < s.size() && s[i] != ',' && s[i] != '}' && s[i] != ']'
           && s[i] != ' ' && s[i] != '\t' && s[i] != '\r' && s[i] != '\n')
        ++i;
}

inline JsonField JsonLookup(const std::string& json, const char* key)
{
    JsonField field;
    size_t i = 0;
    JsonSkipWs(json, i);
    if (i >= json.size() || json[i] != '{')
        return field;
    ++i;
    while (i < json.size()) {
        JsonSkipWs(json, i);
        if (i < json.size() && json[i] == '}')
            break;
        std::string name;
        if (!JsonParseString(json, i, name))
            break;
        JsonSkipWs(json, i);
        if (i >= json.size() || json[i] != ':')
            break;
        ++i;
        JsonSkipWs(json, i);
        if (name == key) {
            field.present = true;
            if (i < json.size() && json[i] == '"') {
                field.isString = true;
                JsonParseString(json, i, field.text);
                if (!field.text.empty()) {
                    char* end = nullptr;
                    field.number = std::strtod(field.text.c_str(), &end);
                    field.boolean = field.text == "true" || field.text == "1";
                }
            } else if (json.compare(i, 4, "true") == 0) {
                field.isBool = true;
                field.boolean = true;
                field.number = 1;
            } else if (json.compare(i, 5, "false") == 0) {
                field.isBool = true;
                field.boolean = false;
            } else if (json.compare(i, 4, "null") == 0) {
                field.present = true;
            } else {
                char* end = nullptr;
                field.number = std::strtod(json.c_str() + i, &end);
            }
            return field;
        }
        JsonSkipValue(json, i);
        JsonSkipWs(json, i);
        if (i < json.size() && json[i] == ',')
            ++i;
    }
    return field;
}

// Copy the raw text of one root-object value, including a nested object or array.
inline bool JsonExtract(const std::string& json, const char* key, std::string& raw)
{
    size_t i = 0;
    JsonSkipWs(json, i);
    if (i >= json.size() || json[i] != '{')
        return false;
    ++i;
    while (i < json.size()) {
        JsonSkipWs(json, i);
        if (i < json.size() && json[i] == '}')
            break;
        std::string name;
        if (!JsonParseString(json, i, name))
            break;
        JsonSkipWs(json, i);
        if (i >= json.size() || json[i] != ':')
            break;
        ++i;
        JsonSkipWs(json, i);
        const size_t start = i;
        JsonSkipValue(json, i);
        if (name == key) {
            raw = json.substr(start, i - start);
            return true;
        }
        JsonSkipWs(json, i);
        if (i < json.size() && json[i] == ',')
            ++i;
    }
    return false;
}

// Split a JSON array into raw element texts. Objects, arrays, and scalars are kept whole.
inline std::vector<std::string> JsonElements(const std::string& raw)
{
    std::vector<std::string> out;
    size_t i = 0;
    JsonSkipWs(raw, i);
    if (i >= raw.size() || raw[i] != '[')
        return out;
    ++i;
    while (i < raw.size()) {
        JsonSkipWs(raw, i);
        if (i < raw.size() && raw[i] == ']')
            break;
        const size_t start = i;
        JsonSkipValue(raw, i);
        if (i > start)
            out.push_back(raw.substr(start, i - start));
        JsonSkipWs(raw, i);
        if (i < raw.size() && raw[i] == ',')
            ++i;
    }
    return out;
}

inline int JsonInt(const JsonField& field, int fallback)
{
    if (!field.present)
        return fallback;
    return static_cast<int>(field.number);
}

inline bool JsonBool(const JsonField& field, bool fallback)
{
    if (!field.present)
        return fallback;
    if (field.isBool)
        return field.boolean;
    if (field.isString)
        return field.boolean || field.number != 0;
    return field.number != 0;
}

inline std::string JsonEscape(const std::string& text)
{
    std::string out;
    out.reserve(text.size() + 8);
    for (unsigned char c : text) {
        switch (c) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\b':
            out += "\\b";
            break;
        case '\f':
            out += "\\f";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (c < 0x20) {
                char buf[8];
                snprintf(buf, sizeof(buf), "\\u%04x", c);
                out += buf;
            } else {
                out.push_back(static_cast<char>(c));
            }
            break;
        }
    }
    return out;
}

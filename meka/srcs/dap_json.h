//-----------------------------------------------------------------------------
// MEKA - dap_json.h
// Minimal JSON builder and parser for DAP (Debug Adapter Protocol)
// No external dependencies.
//-----------------------------------------------------------------------------

#pragma once

#include <string>
#include <vector>
#include <map>
#include <sstream>
#include <stdexcept>

namespace DapJson {

//-----------------------------------------------------------------------------
// JSON value type for parsed data
//-----------------------------------------------------------------------------
enum ValueType {
    JSON_NULL,
    JSON_STRING,
    JSON_NUMBER,
    JSON_BOOL,
    JSON_OBJECT,
    JSON_ARRAY
};

struct Value;

typedef std::map<std::string, Value>    Object;
typedef std::vector<Value>              Array;

struct Value {
    ValueType   type;
    std::string str;
    double      num;
    bool        bol;
    Object      obj;
    Array       arr;

    Value() : type(JSON_NULL), num(0), bol(false) {}
    Value(const std::string& s) : type(JSON_STRING), str(s), num(0), bol(false) {}
    Value(double n) : type(JSON_NUMBER), num(n), bol(false) {}
    Value(bool b) : type(JSON_BOOL), num(0), bol(b) {}
    Value(Object&& o) : type(JSON_OBJECT), num(0), bol(false), obj(std::move(o)) {}
    Value(Array&& a) : type(JSON_ARRAY), num(0), bol(false), arr(std::move(a)) {}

    bool is_null()    const { return type == JSON_NULL; }
    bool is_string()  const { return type == JSON_STRING; }
    bool is_number()  const { return type == JSON_NUMBER; }
    bool is_bool()    const { return type == JSON_BOOL; }
    bool is_object()  const { return type == JSON_OBJECT; }
    bool is_array()   const { return type == JSON_ARRAY; }

    const std::string& as_string() const { return str; }
    double             as_number() const { return num; }
    bool               as_bool()   const { return bol; }
    int                as_int()    const { return (int)num; }

    bool has(const std::string& key) const {
        return is_object() && obj.find(key) != obj.end();
    }
    const Value& operator[](const std::string& key) const {
        static Value null_val;
        if (!is_object()) return null_val;
        auto it = obj.find(key);
        return (it != obj.end()) ? it->second : null_val;
    }
    const Value& operator[](size_t idx) const {
        static Value null_val;
        if (!is_array() || idx >= arr.size()) return null_val;
        return arr[idx];
    }
    size_t size() const { return is_array() ? arr.size() : 0; }
};

//-----------------------------------------------------------------------------
// Escape a string for JSON
//-----------------------------------------------------------------------------
inline std::string escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 2);
    for (char c : s) {
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
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", (unsigned char)c);
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    return out;
}

//-----------------------------------------------------------------------------
// Value to JSON string
//-----------------------------------------------------------------------------
inline std::string to_string(const Value& v) {
    std::ostringstream oss;
    switch (v.type) {
        case JSON_NULL:   return "null";
        case JSON_STRING: return "\"" + escape(v.str) + "\"";
        case JSON_NUMBER: {
            // Print without trailing zeros for integers
            if (v.num == (int)v.num) {
                oss << (int)v.num;
            } else {
                oss << v.num;
            }
            return oss.str();
        }
        case JSON_BOOL:   return v.bol ? "true" : "false";
        case JSON_OBJECT: {
            std::string s = "{";
            bool first = true;
            for (const auto& kv : v.obj) {
                if (!first) s += ",";
                s += "\"" + escape(kv.first) + "\":" + to_string(kv.second);
                first = false;
            }
            return s + "}";
        }
        case JSON_ARRAY: {
            std::string s = "[";
            for (size_t i = 0; i < v.arr.size(); i++) {
                if (i > 0) s += ",";
                s += to_string(v.arr[i]);
            }
            return s + "]";
        }
    }
    return "null";
}

//-----------------------------------------------------------------------------
// Convenience helpers for building JSON objects
//-----------------------------------------------------------------------------
inline Value obj() { return Value(Object{}); }
inline Value arr() { return Value(Array{}); }

inline void set(Value& obj, const char* key, const std::string& val) {
    obj.obj[key] = Value(val);
}
inline void set(Value& obj, const char* key, const char* val) {
    obj.obj[key] = Value(std::string(val));
}
inline void set(Value& obj, const char* key, const Value& val) {
    obj.obj[key] = val;
}
inline void set(Value& obj, const char* key, int val) {
    obj.obj[key] = Value((double)val);
}
inline void set(Value& obj, const char* key, bool val) {
    obj.obj[key] = Value(val);
}
inline void set(Value& obj, const char* key, double val) {
    obj.obj[key] = Value(val);
}
inline void push(Value& arr, const Value& val) {
    arr.arr.push_back(val);
}
inline void push(Value& arr, const char* val) {
    arr.arr.push_back(Value(std::string(val)));
}
inline void push(Value& arr, int val) {
    arr.arr.push_back(Value((double)val));
}
inline void push(Value& arr, const std::string& val) {
    arr.arr.push_back(Value(val));
}

//-----------------------------------------------------------------------------
// JSON Parser - minimal recursive descent
//-----------------------------------------------------------------------------
class Parser {
public:
    Parser(const std::string& input) : s(input), pos(0) {}

    Value parse() {
        skip_ws();
        Value v = parse_value();
        return v;
    }

    const char* error() const { return err; }

private:
    const std::string& s;
    size_t pos;
    const char* err = nullptr;

    char peek() const {
        return (pos < s.size()) ? s[pos] : 0;
    }

    char next() {
        return (pos < s.size()) ? s[pos++] : 0;
    }

    void skip_ws() {
        while (pos < s.size() && (s[pos] == ' ' || s[pos] == '\t' || s[pos] == '\n' || s[pos] == '\r'))
            pos++;
    }

    void expect(char c) {
        skip_ws();
        if (peek() != c) {
            err = "Expected character not found";
            return;
        }
        pos++;
    }

    Value parse_value() {
        skip_ws();
        char c = peek();
        switch (c) {
            case '"':  return parse_string();
            case '{':  return parse_object();
            case '[':  return parse_array();
            case 't': case 'f': return parse_bool();
            case 'n':  return parse_null();
            default:
                if (c == '-' || (c >= '0' && c <= '9'))
                    return parse_number();
                err = "Unexpected character";
                return Value();
        }
    }

    Value parse_string() {
        expect('"');
        std::string out;
        while (pos < s.size()) {
            char c = next();
            if (c == '"') return Value(out);
            if (c == '\\') {
                if (pos >= s.size()) break;
                c = next();
                switch (c) {
                    case '"':  out += '"';  break;
                    case '\\': out += '\\'; break;
                    case '/':  out += '/';  break;
                    case 'b':  out += '\b'; break;
                    case 'f':  out += '\f'; break;
                    case 'n':  out += '\n'; break;
                    case 'r':  out += '\r'; break;
                    case 't':  out += '\t'; break;
                    case 'u': {
                        // Unicode escape - read 4 hex digits
                        unsigned int cp = 0;
                        for (int i = 0; i < 4 && pos < s.size(); i++) {
                            char h = next();
                            cp <<= 4;
                            if (h >= '0' && h <= '9') cp |= (h - '0');
                            else if (h >= 'a' && h <= 'f') cp |= (h - 'a' + 10);
                            else if (h >= 'A' && h <= 'F') cp |= (h - 'A' + 10);
                        }
                        if (cp < 0x80) {
                            out += (char)cp;
                        } else if (cp < 0x800) {
                            out += (char)(0xC0 | (cp >> 6));
                            out += (char)(0x80 | (cp & 0x3F));
                        } else {
                            out += (char)(0xE0 | (cp >> 12));
                            out += (char)(0x80 | ((cp >> 6) & 0x3F));
                            out += (char)(0x80 | (cp & 0x3F));
                        }
                        break;
                    }
                    default:
                        out += c;
                        break;
                }
            } else {
                out += c;
            }
        }
        return Value(out);
    }

    Value parse_number() {
        size_t start = pos;
        if (peek() == '-') pos++;
        while (pos < s.size() && s[pos] >= '0' && s[pos] <= '9') pos++;
        if (pos < s.size() && s[pos] == '.') {
            pos++;
            while (pos < s.size() && s[pos] >= '0' && s[pos] <= '9') pos++;
        }
        if (pos < s.size() && (s[pos] == 'e' || s[pos] == 'E')) {
            pos++;
            if (pos < s.size() && (s[pos] == '+' || s[pos] == '-')) pos++;
            while (pos < s.size() && s[pos] >= '0' && s[pos] <= '9') pos++;
        }
        return Value(std::stod(s.substr(start, pos - start)));
    }

    Value parse_bool() {
        if (s.compare(pos, 4, "true") == 0) { pos += 4; return Value(true); }
        if (s.compare(pos, 5, "false") == 0) { pos += 5; return Value(false); }
        err = "Expected true or false";
        return Value(false);
    }

    Value parse_null() {
        if (s.compare(pos, 4, "null") == 0) { pos += 4; return Value(); }
        err = "Expected null";
        return Value();
    }

    Value parse_object() {
        expect('{');
        Object obj;
        skip_ws();
        if (peek() == '}') { pos++; return Value(std::move(obj)); }
        while (pos < s.size()) {
            skip_ws();
            Value key = parse_string();
            skip_ws();
            expect(':');
            skip_ws();
            Value val = parse_value();
            obj[key.str] = std::move(val);
            skip_ws();
            if (peek() == '}') { pos++; break; }
            expect(',');
        }
        return Value(std::move(obj));
    }

    Value parse_array() {
        expect('[');
        Array arr;
        skip_ws();
        if (peek() == ']') { pos++; return Value(std::move(arr)); }
        while (pos < s.size()) {
            skip_ws();
            arr.push_back(parse_value());
            skip_ws();
            if (peek() == ']') { pos++; break; }
            expect(',');
        }
        return Value(std::move(arr));
    }
};

inline Value parse(const std::string& json) {
    Parser p(json);
    return p.parse();
}

} // namespace DapJson

// Minimal JSON reader for ComfyUI responses (history, upload, prompt replies).
// Read-only DOM; numbers kept as double. Not a general-purpose library.
#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace json {

struct Value {
    enum Type { Null, Bool, Number, String, Array, Object } type = Null;
    bool b = false;
    double n = 0;
    std::string s;
    std::vector<Value> a;
    std::vector<std::pair<std::string, Value>> o;  // keeps key order (ComfyUI output order matters)

    const Value& operator[](const std::string& key) const {
        static const Value none;
        for (auto& kv : o)
            if (kv.first == key) return kv.second;
        return none;
    }
    const Value& operator[](size_t i) const {
        static const Value none;
        return i < a.size() ? a[i] : none;
    }
    bool has(const std::string& key) const {
        for (auto& kv : o)
            if (kv.first == key) return true;
        return false;
    }
    std::string str(const std::string& def = "") const { return type == String ? s : def; }
    double num(double def = 0) const { return type == Number ? n : def; }
};

class Parser {
public:
    explicit Parser(const std::string& t) : t_(t) {}
    bool parse(Value& out) {
        ws();
        if (!value(out)) return false;
        ws();
        return i_ == t_.size();
    }

private:
    const std::string& t_;
    size_t i_ = 0;

    void ws() {
        while (i_ < t_.size() && (t_[i_] == ' ' || t_[i_] == '\n' || t_[i_] == '\r' || t_[i_] == '\t')) ++i_;
    }
    bool lit(const char* w) {
        size_t n = strlen(w);
        if (t_.compare(i_, n, w) != 0) return false;
        i_ += n;
        return true;
    }
    static void utf8(std::string& o, unsigned cp) {
        if (cp < 0x80) o += (char)cp;
        else if (cp < 0x800) { o += (char)(0xC0 | (cp >> 6)); o += (char)(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) { o += (char)(0xE0 | (cp >> 12)); o += (char)(0x80 | ((cp >> 6) & 0x3F)); o += (char)(0x80 | (cp & 0x3F)); }
        else { o += (char)(0xF0 | (cp >> 18)); o += (char)(0x80 | ((cp >> 12) & 0x3F)); o += (char)(0x80 | ((cp >> 6) & 0x3F)); o += (char)(0x80 | (cp & 0x3F)); }
    }
    bool string(std::string& o) {
        if (t_[i_] != '"') return false;
        ++i_;
        while (i_ < t_.size()) {
            char c = t_[i_++];
            if (c == '"') return true;
            if (c != '\\') { o += c; continue; }
            if (i_ >= t_.size()) return false;
            char e = t_[i_++];
            switch (e) {
                case 'n': o += '\n'; break;
                case 't': o += '\t'; break;
                case 'r': o += '\r'; break;
                case 'b': o += '\b'; break;
                case 'f': o += '\f'; break;
                case 'u': {
                    if (i_ + 4 > t_.size()) return false;
                    unsigned cp = std::stoul(t_.substr(i_, 4), nullptr, 16);
                    i_ += 4;
                    if (cp >= 0xD800 && cp < 0xDC00 && t_.compare(i_, 2, "\\u") == 0 && i_ + 6 <= t_.size()) {
                        unsigned lo = std::stoul(t_.substr(i_ + 2, 4), nullptr, 16);
                        i_ += 6;
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    }
                    utf8(o, cp);
                    break;
                }
                default: o += e;
            }
        }
        return false;
    }
    bool value(Value& v) {
        if (i_ >= t_.size()) return false;
        char c = t_[i_];
        if (c == '{') {
            v.type = Value::Object;
            ++i_;
            ws();
            if (t_[i_] == '}') { ++i_; return true; }
            for (;;) {
                ws();
                std::string k;
                if (!string(k)) return false;
                ws();
                if (t_[i_++] != ':') return false;
                ws();
                Value child;
                if (!value(child)) return false;
                v.o.emplace_back(std::move(k), std::move(child));
                ws();
                if (t_[i_] == ',') { ++i_; continue; }
                if (t_[i_] == '}') { ++i_; return true; }
                return false;
            }
        }
        if (c == '[') {
            v.type = Value::Array;
            ++i_;
            ws();
            if (t_[i_] == ']') { ++i_; return true; }
            for (;;) {
                ws();
                Value child;
                if (!value(child)) return false;
                v.a.push_back(std::move(child));
                ws();
                if (t_[i_] == ',') { ++i_; continue; }
                if (t_[i_] == ']') { ++i_; return true; }
                return false;
            }
        }
        if (c == '"') { v.type = Value::String; return string(v.s); }
        if (lit("true")) { v.type = Value::Bool; v.b = true; return true; }
        if (lit("false")) { v.type = Value::Bool; return true; }
        if (lit("null")) { v.type = Value::Null; return true; }
        if (lit("NaN") || lit("Infinity") || lit("-Infinity")) { v.type = Value::Number; return true; }
        size_t start = i_;
        while (i_ < t_.size() && strchr("+-0123456789.eE", t_[i_])) ++i_;
        if (start == i_) return false;
        v.type = Value::Number;
        v.n = std::strtod(t_.substr(start, i_ - start).c_str(), nullptr);
        return true;
    }
};

inline bool parse(const std::string& text, Value& out) {
    try {
        return Parser(text).parse(out);
    } catch (...) {
        return false;
    }
}

// Escape a string as a JSON literal (with quotes).
inline std::string quote(const std::string& s) {
    std::string o = "\"";
    for (unsigned char c : s) {
        switch (c) {
            case '"': o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break;
            case '\r': o += "\\r"; break;
            case '\t': o += "\\t"; break;
            default:
                if (c < 0x20) {
                    char b[8];
                    snprintf(b, sizeof(b), "\\u%04x", c);
                    o += b;
                } else {
                    o += (char)c;
                }
        }
    }
    return o + "\"";
}

}  // namespace json

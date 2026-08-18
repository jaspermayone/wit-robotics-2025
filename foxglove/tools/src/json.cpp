#include "json.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace battlebot::json {

namespace {

/// Formats a double without a trailing ".0" for whole numbers, and without the
/// exponent form that some locales produce. JSON has no NaN or Infinity, so
/// those become 0.
std::string formatNumber(double number) {
    if (!std::isfinite(number)) {
        return "0";
    }
    char text[32];
    std::snprintf(text, sizeof(text), "%.10g", number);
    return text;
}

}  // namespace

std::string escape(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char unicode[7];
                    std::snprintf(unicode, sizeof(unicode), "\\u%04x", c);
                    out += unicode;
                } else {
                    out.push_back(c);
                }
        }
    }
    return out;
}

void Writer::separate() {
    if (need_comma_) {
        buffer_.push_back(',');
    }
    need_comma_ = true;
}

Writer& Writer::key(const std::string& name) {
    separate();
    buffer_.push_back('"');
    buffer_ += escape(name);
    buffer_ += "\":";
    // The value that follows belongs to this key, so suppress the comma once.
    need_comma_ = false;
    return *this;
}

Writer& Writer::value(double number) {
    buffer_ += formatNumber(number);
    need_comma_ = true;
    return *this;
}

Writer& Writer::value(bool flag) {
    buffer_ += flag ? "true" : "false";
    need_comma_ = true;
    return *this;
}

Writer& Writer::value(const std::string& text) {
    buffer_.push_back('"');
    buffer_ += escape(text);
    buffer_.push_back('"');
    need_comma_ = true;
    return *this;
}

Writer& Writer::beginObject(const std::string& name) {
    key(name);
    buffer_.push_back('{');
    need_comma_ = false;
    return *this;
}

Writer& Writer::endObject() {
    buffer_.push_back('}');
    need_comma_ = true;
    return *this;
}

std::string Writer::finish() {
    buffer_.push_back('}');
    return buffer_;
}

}  // namespace battlebot::json

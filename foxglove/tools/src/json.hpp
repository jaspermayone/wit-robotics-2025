#pragma once

// A small JSON writer.
//
// The generator only writes flat JSON messages, so a full JSON library is not
// needed.

#include <string>

namespace battlebot::json {

/// Builds a JSON object into a string buffer.
class Writer {
public:
    Writer() { buffer_ = "{"; }

    Writer& key(const std::string& name);
    Writer& value(double number);
    Writer& value(bool flag);
    Writer& value(const std::string& text);

    /// Starts a nested object under the given key.
    Writer& beginObject(const std::string& name);
    /// Closes the innermost nested object.
    Writer& endObject();

    /// Closes the outermost object and returns the finished document.
    std::string finish();

private:
    void separate();

    std::string buffer_;
    bool need_comma_ = false;
};

/// Escapes a string for use as a JSON string value, without the quotes.
std::string escape(const std::string& text);

}  // namespace battlebot::json

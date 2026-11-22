#pragma once
// Minimal, defensive JSON (RFC 8259 subset) for the viewer's own files (.gsvproj, settings).
// Parser: depth-limited (no stack overflow on hostile input), rejects trailing data, non-finite
// numbers and bad escapes with a message. \u escapes are decoded to UTF-8 (BMP).

#include <string>
#include <utility>
#include <vector>

namespace gsv {

struct Json {
  enum class Type { Null, Bool, Number, String, Array, Object } type = Type::Null;
  bool b = false;
  double n = 0.0;
  std::string s;
  std::vector<Json> a;
  std::vector<std::pair<std::string, Json>> o;
  const Json* get(const char* key) const {
    for (const auto& kv : o)
      if (kv.first == key) return &kv.second;
    return nullptr;
  }
  bool isNumber() const { return type == Type::Number; }
  bool isString() const { return type == Type::String; }
  bool isArray() const { return type == Type::Array; }
  bool isObject() const { return type == Type::Object; }
};

bool parseJson(const std::string& text, Json& out, std::string& error, int maxDepth = 64);
std::string jsonQuote(const std::string& s);  // with the quotes
std::string jsonNumber(double v);              // round-trips (%.17g)

}  // namespace gsv

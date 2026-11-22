#include "core/json.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace gsv {

namespace {

class Parser {
 public:
  Parser(const std::string& t, int maxDepth) : t_(t), maxDepth_(maxDepth) {}
  bool parse(Json& out, std::string& err) {
    if (!value(out, 0)) {
      err = error_.empty() ? "invalid JSON" : error_;
      err += " at byte " + std::to_string(pos_);
      return false;
    }
    ws();
    if (pos_ != t_.size()) {
      err = "trailing characters at byte " + std::to_string(pos_);
      return false;
    }
    return true;
  }

 private:
  void ws() {
    while (pos_ < t_.size() && (t_[pos_] == ' ' || t_[pos_] == '\t' || t_[pos_] == '\n' || t_[pos_] == '\r')) ++pos_;
  }
  bool fail(const char* m) {
    if (error_.empty()) error_ = m;
    return false;
  }
  bool value(Json& v, int depth) {
    if (depth > maxDepth_) return fail("nesting too deep");
    ws();
    if (pos_ >= t_.size()) return fail("unexpected end");
    const char c = t_[pos_];
    if (c == '{') return object(v, depth);
    if (c == '[') return array(v, depth);
    if (c == '"') {
      v.type = Json::Type::String;
      return string(v.s);
    }
    if (t_.compare(pos_, 4, "true") == 0) { pos_ += 4; v.type = Json::Type::Bool; v.b = true; return true; }
    if (t_.compare(pos_, 5, "false") == 0) { pos_ += 5; v.type = Json::Type::Bool; v.b = false; return true; }
    if (t_.compare(pos_, 4, "null") == 0) { pos_ += 4; v.type = Json::Type::Null; return true; }
    return number(v);
  }
  bool number(Json& v) {
    const size_t start = pos_;
    while (pos_ < t_.size() && std::strchr("+-0123456789.eE", t_[pos_])) ++pos_;
    if (pos_ == start) return fail("unexpected character");
    const std::string s = t_.substr(start, pos_ - start);
    char* end = nullptr;
    v.n = std::strtod(s.c_str(), &end);
    if (!end || *end != '\0' || !std::isfinite(v.n)) return fail("bad number");
    v.type = Json::Type::Number;
    return true;
  }
  bool string(std::string& out) {
    ++pos_;  // opening quote
    out.clear();
    while (pos_ < t_.size()) {
      const char c = t_[pos_++];
      if (c == '"') return true;
      if (c != '\\') {
        out.push_back(c);
        continue;
      }
      if (pos_ >= t_.size()) break;
      const char e = t_[pos_++];
      switch (e) {
        case '"': out.push_back('"'); break;
        case '\\': out.push_back('\\'); break;
        case '/': out.push_back('/'); break;
        case 'n': out.push_back('\n'); break;
        case 't': out.push_back('\t'); break;
        case 'r': out.push_back('\r'); break;
        case 'b': out.push_back('\b'); break;
        case 'f': out.push_back('\f'); break;
        case 'u': {
          if (pos_ + 4 > t_.size()) return fail("bad \\u escape");
          unsigned cp = 0;
          for (int k = 0; k < 4; ++k) {
            const char h = t_[pos_++];
            cp <<= 4;
            if (h >= '0' && h <= '9') cp |= static_cast<unsigned>(h - '0');
            else if (h >= 'a' && h <= 'f') cp |= static_cast<unsigned>(h - 'a' + 10);
            else if (h >= 'A' && h <= 'F') cp |= static_cast<unsigned>(h - 'A' + 10);
            else return fail("bad \\u escape");
          }
          // UTF-8 encode (BMP only; surrogate pairs are kept as two code points)
          if (cp < 0x80) out.push_back(static_cast<char>(cp));
          else if (cp < 0x800) { out.push_back(static_cast<char>(0xC0 | (cp >> 6))); out.push_back(static_cast<char>(0x80 | (cp & 0x3F))); }
          else { out.push_back(static_cast<char>(0xE0 | (cp >> 12))); out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F))); out.push_back(static_cast<char>(0x80 | (cp & 0x3F))); }
          break;
        }
        default: return fail("bad escape");
      }
    }
    return fail("unterminated string");
  }
  bool array(Json& v, int depth) {
    ++pos_;
    v.type = Json::Type::Array;
    ws();
    if (pos_ < t_.size() && t_[pos_] == ']') { ++pos_; return true; }
    while (true) {
      v.a.emplace_back();
      if (!value(v.a.back(), depth + 1)) return false;
      ws();
      if (pos_ >= t_.size()) return fail("unterminated array");
      if (t_[pos_] == ',') { ++pos_; continue; }
      if (t_[pos_] == ']') { ++pos_; return true; }
      return fail("expected , or ]");
    }
  }
  bool object(Json& v, int depth) {
    ++pos_;
    v.type = Json::Type::Object;
    ws();
    if (pos_ < t_.size() && t_[pos_] == '}') { ++pos_; return true; }
    while (true) {
      ws();
      if (pos_ >= t_.size() || t_[pos_] != '"') return fail("expected key");
      std::string key;
      if (!string(key)) return false;
      ws();
      if (pos_ >= t_.size() || t_[pos_] != ':') return fail("expected :");
      ++pos_;
      v.o.emplace_back(key, Json());
      if (!value(v.o.back().second, depth + 1)) return false;
      ws();
      if (pos_ >= t_.size()) return fail("unterminated object");
      if (t_[pos_] == ',') { ++pos_; continue; }
      if (t_[pos_] == '}') { ++pos_; return true; }
      return fail("expected , or }");
    }
  }
  const std::string& t_;
  size_t pos_ = 0;
  int maxDepth_ = 64;
  std::string error_;
};

}  // namespace

std::string jsonQuote(const std::string& s) {
  std::string o = "\"";
  for (unsigned char c : s) {
    if (c == '"') o += "\\\"";
    else if (c == '\\') o += "\\\\";
    else if (c == '\n') o += "\\n";
    else if (c == '\t') o += "\\t";
    else if (c == '\r') o += "\\r";
    else if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); o += b; }
    else o.push_back(static_cast<char>(c));
  }
  return o + "\"";
}
std::string jsonNumber(double v) {
  char b[40];
  std::snprintf(b, sizeof b, "%.17g", v);
  return b;
}

bool parseJson(const std::string& text, Json& out, std::string& error, int maxDepth) {
  out = Json{};
  Parser p(text, maxDepth);
  return p.parse(out, error);
}

}  // namespace gsv

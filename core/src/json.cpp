#include "panplay/json.hpp"

#include <cctype>

namespace panplay {
namespace {

class Parser {
 public:
  explicit Parser(const std::string& input) : input_(input) {}

  Json parse_value() {
    skip();
    if (pos_ >= input_.size()) {
      fail("unexpected end");
      return {};
    }
    const char c = input_[pos_];
    if (c == '{') return parse_object();
    if (c == '[') return parse_array();
    if (c == '"') return parse_string();
    if (c == 't' || c == 'f') return parse_bool();
    if (c == 'n') return parse_null();
    if (c == '-' || std::isdigit(static_cast<unsigned char>(c))) return parse_number();
    fail("unexpected character");
    return {};
  }

  bool ok() const { return error_.empty(); }
  const std::string& error() const { return error_; }

 private:
  void skip() {
    while (pos_ < input_.size() && std::isspace(static_cast<unsigned char>(input_[pos_]))) ++pos_;
  }

  void fail(const std::string& message) {
    if (error_.empty()) error_ = message;
  }

  bool consume(char expected) {
    skip();
    if (pos_ < input_.size() && input_[pos_] == expected) {
      ++pos_;
      return true;
    }
    fail(std::string("expected ") + expected);
    return false;
  }

  Json parse_object() {
    Json value;
    value.type = Json::Type::object;
    if (!consume('{')) return {};
    skip();
    if (pos_ < input_.size() && input_[pos_] == '}') {
      ++pos_;
      return value;
    }
    while (ok()) {
      Json key = parse_string();
      if (!ok() || !consume(':')) break;
      Json child = parse_value();
      if (!ok()) break;
      value.object.emplace(key.text, std::move(child));
      skip();
      if (pos_ < input_.size() && input_[pos_] == ',') {
        ++pos_;
        continue;
      }
      if (!consume('}')) return {};
      return value;
    }
    return {};
  }

  Json parse_array() {
    Json value;
    value.type = Json::Type::array;
    if (!consume('[')) return {};
    skip();
    if (pos_ < input_.size() && input_[pos_] == ']') {
      ++pos_;
      return value;
    }
    while (ok()) {
      value.array.push_back(parse_value());
      if (!ok()) break;
      skip();
      if (pos_ < input_.size() && input_[pos_] == ',') {
        ++pos_;
        continue;
      }
      if (!consume(']')) return {};
      return value;
    }
    return {};
  }

  Json parse_string() {
    Json value;
    value.type = Json::Type::string;
    if (!consume('"')) return {};
    std::string out;
    while (pos_ < input_.size()) {
      char c = input_[pos_++];
      if (c == '"') {
        value.text = std::move(out);
        return value;
      }
      if (c != '\\') {
        out.push_back(c);
        continue;
      }
      if (pos_ >= input_.size()) {
        fail("bad escape");
        return {};
      }
      char e = input_[pos_++];
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
        case 'u': {
          if (pos_ + 4 > input_.size()) {
            fail("bad unicode escape");
            return {};
          }
          int code = 0;
          for (int i = 0; i < 4; ++i) {
            char h = input_[pos_++];
            code <<= 4;
            if (h >= '0' && h <= '9') code += h - '0';
            else if (h >= 'a' && h <= 'f') code += h - 'a' + 10;
            else if (h >= 'A' && h <= 'F') code += h - 'A' + 10;
            else {
              fail("bad unicode escape");
              return {};
            }
          }
          append_utf8(out, code);
          break;
        }
        default:
          fail("bad escape");
          return {};
      }
    }
    fail("unterminated string");
    return {};
  }

  static void append_utf8(std::string& out, int code) {
    if (code < 0x80) {
      out.push_back(static_cast<char>(code));
    } else if (code < 0x800) {
      out.push_back(static_cast<char>(0xC0 | (code >> 6)));
      out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
    } else {
      out.push_back(static_cast<char>(0xE0 | (code >> 12)));
      out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
    }
  }

  Json parse_bool() {
    Json value;
    value.type = Json::Type::boolean;
    if (input_.compare(pos_, 4, "true") == 0) {
      pos_ += 4;
      value.boolean = true;
      return value;
    }
    if (input_.compare(pos_, 5, "false") == 0) {
      pos_ += 5;
      value.boolean = false;
      return value;
    }
    fail("expected boolean");
    return {};
  }

  Json parse_null() {
    if (input_.compare(pos_, 4, "null") != 0) {
      fail("expected null");
      return {};
    }
    pos_ += 4;
    return {};
  }

  Json parse_number() {
    Json value;
    value.type = Json::Type::number;
    const std::size_t start = pos_;
    if (input_[pos_] == '-') ++pos_;
    while (pos_ < input_.size() && (std::isdigit(static_cast<unsigned char>(input_[pos_])) || input_[pos_] == '.' ||
                                    input_[pos_] == 'e' || input_[pos_] == 'E' || input_[pos_] == '+' ||
                                    input_[pos_] == '-')) {
      ++pos_;
    }
    value.text = input_.substr(start, pos_ - start);
    try {
      value.number = std::stod(value.text);
    } catch (...) {
      fail("bad number");
    }
    return value;
  }

  const std::string& input_;
  std::size_t pos_ = 0;
  std::string error_;
};

}  // namespace

Json Json::parse(const std::string& input, std::string* error) {
  Parser parser(input);
  Json value = parser.parse_value();
  if (!parser.ok()) {
    if (error) *error = parser.error();
    return {};
  }
  return value;
}

const Json* Json::get(const std::string& key) const {
  if (type != Type::object) return nullptr;
  auto it = object.find(key);
  if (it == object.end()) return nullptr;
  return &it->second;
}

std::string Json::str(const std::string& key) const {
  const Json* child = get(key);
  if (!child) return {};
  if (child->type == Type::string) return child->text;
  if (child->type == Type::number) return child->text;
  return {};
}

double Json::num(const std::string& key) const {
  const Json* child = get(key);
  if (!child || child->type != Type::number) return 0;
  return child->number;
}

bool Json::boolean_or(const std::string& key, bool fallback) const {
  const Json* child = get(key);
  if (!child || child->type != Type::boolean) return fallback;
  return child->boolean;
}

std::string json_escape(const std::string& value) {
  std::string out;
  out.reserve(value.size());
  for (unsigned char c : value) {
    switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\n':
        out += "\\n";
        break;
      default:
        if (c < 0x20) {
          static const char hex[] = "0123456789abcdef";
          out += "\\u00";
          out.push_back(hex[c >> 4]);
          out.push_back(hex[c & 0xF]);
        } else {
          out.push_back(static_cast<char>(c));
        }
        break;
    }
  }
  return out;
}

}  // namespace panplay

#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace panplay {

struct Json {
  enum class Type { null, boolean, number, string, array, object };

  Type type = Type::null;
  bool boolean = false;
  double number = 0;
  std::string text;
  std::vector<Json> array;
  std::map<std::string, Json> object;

  static Json parse(const std::string& input, std::string* error);

  const Json* get(const std::string& key) const;
  std::string str(const std::string& key) const;
  double num(const std::string& key) const;
  bool boolean_or(const std::string& key, bool fallback) const;
};

std::string json_escape(const std::string& value);

}  // namespace panplay

#pragma once

#include <string>
#include <utility>
#include <vector>

namespace panplay {

struct HttpResponse {
  int status = 0;
  std::string body;
  std::string error;
};

struct HttpRequest {
  std::string url;
  std::string method = "GET";
  std::string body;
  std::vector<std::pair<std::string, std::string>> headers;
  int timeout_ms = 0;  // 0 = default (30s receive)
};

HttpResponse http_send(const HttpRequest& request);

std::string url_encode(const std::string& value);

}  // namespace panplay

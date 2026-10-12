#include "panplay/http.hpp"

#include <cctype>

namespace panplay {

std::string url_encode(const std::string& value) {
  static const char* hex = "0123456789ABCDEF";
  std::string out;
  for (unsigned char c : value) {
    if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      out.push_back(static_cast<char>(c));
    } else {
      out.push_back('%');
      out.push_back(hex[c >> 4]);
      out.push_back(hex[c & 0x0F]);
    }
  }
  return out;
}

HttpResponse HttpClient::send(const HttpRequest& request) {
  HttpResponse response;
  auto stream = open(request, &response.error);
  if (!stream) return response;
  response.status = stream->status();
  response.final_url = stream->final_url();
  std::uint8_t buffer[65536];
  for (;;) {
    std::string error;
    const std::size_t got = stream->read(buffer, sizeof(buffer), &error);
    if (!error.empty()) {
      response.error = error;
      break;
    }
    if (got == 0) break;
    response.body.append(reinterpret_cast<const char*>(buffer), got);
  }
  return response;
}

HttpResponse http_send(const HttpRequest& request) {
  static HttpClient client;
  return client.send(request);
}

}  // namespace panplay

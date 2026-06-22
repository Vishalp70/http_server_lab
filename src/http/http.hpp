#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace srv {

inline constexpr std::size_t kMaxHttpRequestBytes = 8192;

struct HttpRequest {
  std::string method;
  std::string target;
  std::string version;
  std::unordered_map<std::string, std::string> headers;
};

enum class HttpParseError { kNone, kMalformed, kTooLarge };

struct HttpParseResult {
  std::optional<HttpRequest> request;
  HttpParseError error = HttpParseError::kNone;
};

// Parses exactly one complete header-only HTTP/1.1 request.
HttpParseResult parse_http_request(std::string_view bytes);

// Applies the shared GET application routes and serializes one close response.
std::string handle_http_request(std::string_view bytes);
std::string make_service_unavailable_response();

}  // namespace srv

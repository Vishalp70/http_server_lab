#include "http/http.hpp"

#include <algorithm>
#include <charconv>
#include <string>
#include <system_error>

namespace srv {
namespace {

bool is_token_char(unsigned char c) {
  if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
    return true;
  }
  constexpr std::string_view punctuation = "!#$%&'*+-.^_`|~";
  return punctuation.find(static_cast<char>(c)) != std::string_view::npos;
}

std::string trim_ows(std::string_view value) {
  while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
    value.remove_prefix(1);
  }
  while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) {
    value.remove_suffix(1);
  }
  return std::string(value);
}

std::string lower_ascii(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](char c) {
    if (c >= 'A' && c <= 'Z') {
      return static_cast<char>(c - 'A' + 'a');
    }
    return c;
  });
  return value;
}

std::string response(int status, std::string_view reason, std::string_view body,
                     std::string_view extra_headers = {}) {
  std::string result = "HTTP/1.1 " + std::to_string(status) + " ";
  result += reason;
  result += "\r\nContent-Type: text/plain; charset=utf-8\r\nContent-Length: ";
  result += std::to_string(body.size());
  result += "\r\nConnection: close\r\n";
  result += extra_headers;
  result += "\r\n";
  result += body;
  return result;
}

}  // namespace

HttpParseResult parse_http_request(std::string_view bytes) {
  if (bytes.size() > kMaxHttpRequestBytes) {
    return {{}, HttpParseError::kTooLarge};
  }
  const std::size_t header_end = bytes.find("\r\n\r\n");
  if (header_end == std::string_view::npos || header_end + 4 != bytes.size()) {
    return {{}, HttpParseError::kMalformed};
  }
  if (bytes.find('\0') != std::string_view::npos) {
    return {{}, HttpParseError::kMalformed};
  }

  HttpRequest request;
  const std::size_t first_line_end = bytes.find("\r\n");
  if (first_line_end == std::string_view::npos || first_line_end == 0) {
    return {{}, HttpParseError::kMalformed};
  }
  const std::string_view request_line = bytes.substr(0, first_line_end);
  const std::size_t first_space = request_line.find(' ');
  const std::size_t second_space = first_space == std::string_view::npos
                                       ? std::string_view::npos
                                       : request_line.find(' ', first_space + 1);
  if (first_space == std::string_view::npos || second_space == std::string_view::npos ||
      first_space == 0 || second_space == first_space + 1 ||
      request_line.find(' ', second_space + 1) != std::string_view::npos) {
    return {{}, HttpParseError::kMalformed};
  }
  const std::string_view method = request_line.substr(0, first_space);
  if (!std::all_of(method.begin(), method.end(), [](unsigned char c) { return is_token_char(c); })) {
    return {{}, HttpParseError::kMalformed};
  }
  request.method = std::string(method);
  request.target = std::string(request_line.substr(first_space + 1, second_space - first_space - 1));
  request.version = std::string(request_line.substr(second_space + 1));
  if (request.target.empty() || request.target.front() != '/' || request.version != "HTTP/1.1") {
    return {{}, HttpParseError::kMalformed};
  }
  if (!std::all_of(request.target.begin(), request.target.end(), [](unsigned char c) {
        return c >= 0x21 && c <= 0x7e;
      })) {
    return {{}, HttpParseError::kMalformed};
  }

  std::size_t line_begin = first_line_end + 2;
  while (line_begin < header_end) {
    const std::size_t line_end = bytes.find("\r\n", line_begin);
    if (line_end == std::string_view::npos || line_end > header_end || line_end == line_begin) {
      return {{}, HttpParseError::kMalformed};
    }
    const std::string_view line = bytes.substr(line_begin, line_end - line_begin);
    const std::size_t colon = line.find(':');
    if (colon == std::string_view::npos || colon == 0) {
      return {{}, HttpParseError::kMalformed};
    }
    const std::string_view name = line.substr(0, colon);
    if (!std::all_of(name.begin(), name.end(), [](unsigned char c) { return is_token_char(c); })) {
      return {{}, HttpParseError::kMalformed};
    }
    const std::string key = lower_ascii(std::string(name));
    const std::string value = trim_ows(line.substr(colon + 1));
    if (std::any_of(value.begin(), value.end(), [](unsigned char c) {
          return (c < 0x20 && c != '\t') || c == 0x7f;
        })) {
      return {{}, HttpParseError::kMalformed};
    }
    if (!request.headers.emplace(key, value).second) {
      return {{}, HttpParseError::kMalformed};
    }
    line_begin = line_end + 2;
  }

  const auto host = request.headers.find("host");
  if (host == request.headers.end() || host->second.empty() ||
      request.headers.find("transfer-encoding") != request.headers.end()) {
    return {{}, HttpParseError::kMalformed};
  }
  const auto length = request.headers.find("content-length");
  if (length != request.headers.end()) {
    std::size_t body_size = 0;
    const char* first = length->second.data();
    const char* last = first + length->second.size();
    const auto [ptr, error] = std::from_chars(first, last, body_size);
    if (error != std::errc{} || ptr != last || body_size != 0) {
      return {{}, HttpParseError::kMalformed};
    }
  }
  return {std::move(request), HttpParseError::kNone};
}

std::string handle_http_request(std::string_view bytes) {
  const HttpParseResult parsed = parse_http_request(bytes);
  if (!parsed.request) {
    if (parsed.error == HttpParseError::kTooLarge) {
      return response(431, "Request Header Fields Too Large", "Request too large\n");
    }
    return response(400, "Bad Request", "Bad request\n");
  }
  const HttpRequest& request = *parsed.request;
  if (request.method != "GET") {
    return response(405, "Method Not Allowed", "Method not allowed\n", "Allow: GET\r\n");
  }
  if (request.target == "/") {
    return response(200, "OK", "Linux HTTP server lab\n");
  }
  if (request.target == "/health") {
    return response(200, "OK", "OK\n");
  }
  if (request.target == "/hello") {
    return response(200, "OK", "Hello!\n");
  }
  return response(404, "Not Found", "Not found\n");
}

std::string make_service_unavailable_response() {
  return response(503, "Service Unavailable", "Server busy\n");
}

}  // namespace srv

#include <gtest/gtest.h>

#include <string>

#include "http/http.hpp"

namespace srv {
namespace {

TEST(HttpParser, ParsesRequestLineAndCaseInsensitiveHeaders) {
  const HttpParseResult parsed =
      parse_http_request("GET /health HTTP/1.1\r\nHost: example.test\r\nX-Mode:  yes \t\r\n\r\n");
  ASSERT_TRUE(parsed.request);
  EXPECT_EQ(parsed.request->method, "GET");
  EXPECT_EQ(parsed.request->target, "/health");
  EXPECT_EQ(parsed.request->version, "HTTP/1.1");
  EXPECT_EQ(parsed.request->headers.at("host"), "example.test");
  EXPECT_EQ(parsed.request->headers.at("x-mode"), "yes");
}

TEST(HttpParser, RejectsIncompleteAndMalformedRequestLines) {
  EXPECT_EQ(parse_http_request("GET / HTTP/1.1\r\nHost: x\r\n").error,
            HttpParseError::kMalformed);
  EXPECT_EQ(parse_http_request("GET  HTTP/1.1\r\nHost: x\r\n\r\n").error,
            HttpParseError::kMalformed);
  EXPECT_EQ(parse_http_request("GET / HTTP/1.0\r\nHost: x\r\n\r\n").error,
            HttpParseError::kMalformed);
}

TEST(HttpParser, RequiresHostAndRejectsAmbiguousHeaders) {
  EXPECT_FALSE(parse_http_request("GET / HTTP/1.1\r\n\r\n").request);
  EXPECT_FALSE(parse_http_request(
      "GET / HTTP/1.1\r\nHost: a\r\nhost: b\r\n\r\n").request);
  EXPECT_FALSE(parse_http_request(
      "GET / HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: chunked\r\n\r\n").request);
}

TEST(HttpParser, RejectsNonzeroBodyAndTrailingBytes) {
  EXPECT_FALSE(parse_http_request(
      "GET / HTTP/1.1\r\nHost: a\r\nContent-Length: 1\r\n\r\nx").request);
  EXPECT_FALSE(parse_http_request("GET / HTTP/1.1\r\nHost: a\r\n\r\nextra").request);
}

TEST(HttpParser, EnforcesRequestSizeLimit) {
  const std::string oversized(kMaxHttpRequestBytes + 1, 'x');
  EXPECT_EQ(parse_http_request(oversized).error, HttpParseError::kTooLarge);
}

TEST(HttpApplication, ServesThreeGetRoutes) {
  const std::string root = handle_http_request("GET / HTTP/1.1\r\nHost: x\r\n\r\n");
  const std::string health = handle_http_request("GET /health HTTP/1.1\r\nHost: x\r\n\r\n");
  const std::string hello = handle_http_request("GET /hello HTTP/1.1\r\nHost: x\r\n\r\n");
  EXPECT_NE(root.find("HTTP/1.1 200 OK\r\n"), std::string::npos);
  EXPECT_NE(root.find("Linux HTTP server lab\n"), std::string::npos);
  EXPECT_NE(health.find("\r\n\r\nOK\n"), std::string::npos);
  EXPECT_NE(hello.find("\r\n\r\nHello!\n"), std::string::npos);
}

TEST(HttpApplication, ReturnsErrorsAndClosesEveryResponse) {
  const std::string malformed = handle_http_request("not a request\r\n\r\n");
  const std::string missing = handle_http_request("GET /missing HTTP/1.1\r\nHost: x\r\n\r\n");
  const std::string method = handle_http_request("POST / HTTP/1.1\r\nHost: x\r\n\r\n");
  const std::string oversized = handle_http_request(std::string(kMaxHttpRequestBytes + 1, 'x'));
  EXPECT_NE(malformed.find("HTTP/1.1 400 Bad Request\r\n"), std::string::npos);
  EXPECT_NE(missing.find("HTTP/1.1 404 Not Found\r\n"), std::string::npos);
  EXPECT_NE(method.find("HTTP/1.1 405 Method Not Allowed\r\n"), std::string::npos);
  EXPECT_NE(method.find("Allow: GET\r\n"), std::string::npos);
  EXPECT_NE(oversized.find("HTTP/1.1 431 Request Header Fields Too Large\r\n"),
            std::string::npos);
  EXPECT_NE(missing.find("Connection: close\r\n"), std::string::npos);
}

}  // namespace
}  // namespace srv

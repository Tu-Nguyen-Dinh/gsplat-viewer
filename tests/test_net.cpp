// Tests for the LAN networking used by gsplat_server and the Android app (src/core/net.*).
#include "core/net.h"
#include "test_framework.h"

#include <string>
#include <thread>

using namespace gsv;

TEST_CASE(net_url_encoding_and_query) {
  CHECK(net::urlDecode("a%20b+c%2Fd") == "a b c/d");
  CHECK(net::urlEncode("files/my scene.ply") == "files/my%20scene.ply");
  CHECK(net::urlDecode(net::urlEncode("thử nghiệm")) == "thử nghiệm");
  CHECK(net::queryValue("name=desk&ext=mp4", "ext") == "mp4");
  CHECK(net::queryValue("name=my%20desk", "name") == "my desk");
  CHECK(net::queryValue("name=desk", "missing").empty());
}

// A real round trip over loopback: request parsing, response, client with a streamed body.
TEST_CASE(net_http_roundtrip_on_loopback) {
  std::string error;
  net::Socket server;
  uint16_t port = 0;
  for (uint16_t p = 18901; p < 18920 && !server.valid(); ++p) server = net::listenTcp(port = p, error);
  REQUIRE(server.valid());
  const std::string payload(300000, 'x');  // larger than one receive buffer
  std::string seenPath, seenQuery;
  std::thread t([&] {
    net::Socket c = net::acceptClient(server);
    net::Request r;
    if (net::readRequest(c, r)) {
      seenPath = r.path;
      seenQuery = r.query;
      net::sendResponse(c, 200, "text/plain", payload);
    }
  });
  net::Response resp;
  size_t chunks = 0, bytes = 0;
  const bool ok = net::httpGet("127.0.0.1", port, "/files/a b/c.ply?name=x%20y&k=1", resp, error,
                               [&](const char*, size_t n) {
                                 ++chunks;
                                 bytes += n;
                                 return true;
                               });
  t.join();
  CHECK(ok);
  CHECK(resp.status == 200);
  CHECK(bytes == payload.size());
  CHECK(chunks >= 1);
  CHECK(seenPath == "/files/a b/c.ply");  // encoded by the client, decoded by the server
  // The query reaches the server as a query (it used to be percent-encoded into the path).
  CHECK(net::queryValue(seenQuery, "name") == "x y");
  CHECK(net::queryValue(seenQuery, "k") == "1");
  CHECK(resp.headers["content-type"] == "text/plain");
}

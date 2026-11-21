#pragma once
// Minimal networking for the capture workflow (phone <-> PC on the same Wi-Fi):
// TCP sockets, UDP broadcast discovery and just enough HTTP/1.1 for a small file server and its
// client. Winsock on Windows, BSD sockets elsewhere (Linux, Android). No TLS: LAN only.

#include <cstdint>
#include <functional>
#include <map>
#include <string>

namespace gsv::net {

constexpr uint16_t kHttpPort = 8765;       // gsplat_server
constexpr uint16_t kDiscoveryPort = 8766;  // UDP: "GSPLAT_DISCOVER" -> "GSPLAT_SERVER <http port>"

bool init();  // WSAStartup on Windows; safe to call more than once

class Socket {
 public:
  Socket() = default;
  explicit Socket(intptr_t handle) : handle_(handle) {}
  ~Socket() { close(); }
  Socket(Socket&& o) noexcept : handle_(o.handle_) { o.handle_ = -1; }
  Socket& operator=(Socket&& o) noexcept;
  Socket(const Socket&) = delete;
  Socket& operator=(const Socket&) = delete;

  bool valid() const { return handle_ >= 0; }
  intptr_t handle() const { return handle_; }
  void close();

  bool sendAll(const void* data, size_t size);
  bool sendAll(const std::string& s) { return sendAll(s.data(), s.size()); }
  // Up to `size` bytes; 0 = connection closed, < 0 = error or timeout.
  long receive(void* data, size_t size);
  void setTimeout(int milliseconds);

 private:
  intptr_t handle_ = -1;
};

Socket listenTcp(uint16_t port, std::string& error);  // all interfaces
Socket acceptClient(const Socket& server, std::string* peer = nullptr);
Socket connectTcp(const std::string& host, uint16_t port, int timeoutMs, std::string& error);

// Discovery. The server answers broadcasts on kDiscoveryPort; the client broadcasts and returns
// the first answer as "a.b.c.d" (empty if nothing answered within timeoutMs).
void serveDiscovery(uint16_t httpPort, const bool& stop);  // blocking loop, run on a thread
std::string discoverServer(int timeoutMs, uint16_t* httpPort = nullptr);

// ---- HTTP ------------------------------------------------------------------------------------
struct Request {
  std::string method, path, query;          // path without the query string, URL-decoded
  std::map<std::string, std::string> headers;  // keys in lower case
  std::string bodyStart;                    // body bytes already read with the header
  long long contentLength = -1;
};

// Reads the request line and headers (up to the blank line).
bool readRequest(Socket& s, Request& r);
std::string queryValue(const std::string& query, const std::string& key);  // URL-decoded
std::string urlDecode(const std::string& s);
std::string urlEncode(const std::string& s);

bool sendResponse(Socket& s, int status, const std::string& contentType, const std::string& body);
// Streams a file; 404 if it cannot be opened.
bool sendFile(Socket& s, const std::string& utf8Path, const std::string& contentType);

// Client: GET http://host:port/path. Body goes to `onData` (chunks) or into `body`.
struct Response {
  int status = 0;
  std::map<std::string, std::string> headers;
  std::string body;
};
bool httpGet(const std::string& host, uint16_t port, const std::string& path, Response& out, std::string& error,
             const std::function<bool(const char*, size_t)>& onData = {}, int timeoutMs = 10000);

}  // namespace gsv::net

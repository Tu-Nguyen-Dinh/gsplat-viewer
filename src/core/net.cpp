#include "core/net.h"

#include "core/utf8_path.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
using SocketLength = int;
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
using SocketLength = socklen_t;
#endif

namespace gsv::net {

namespace {

#if defined(_WIN32)
void closeHandle(intptr_t h) { closesocket(static_cast<SOCKET>(h)); }
#else
void closeHandle(intptr_t h) { ::close(static_cast<int>(h)); }
#endif

std::string lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}

const char* statusText(int status) {
  switch (status) {
    case 200: return "OK";
    case 400: return "Bad Request";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 409: return "Conflict";
    case 413: return "Payload Too Large";
    default: return "Error";
  }
}

}  // namespace

bool init() {
#if defined(_WIN32)
  static const bool ok = [] {
    WSADATA data;
    return WSAStartup(MAKEWORD(2, 2), &data) == 0;
  }();
  return ok;
#else
  return true;
#endif
}

Socket& Socket::operator=(Socket&& o) noexcept {
  if (this != &o) {
    close();
    handle_ = o.handle_;
    o.handle_ = -1;
  }
  return *this;
}

void Socket::close() {
  if (handle_ >= 0) closeHandle(handle_);
  handle_ = -1;
}

bool Socket::sendAll(const void* data, size_t size) {
  const char* p = static_cast<const char*>(data);
  while (size > 0) {
    const int chunk = static_cast<int>(std::min<size_t>(size, 1 << 20));
#if defined(_WIN32)
    const int n = ::send(static_cast<SOCKET>(handle_), p, chunk, 0);
#else
    const long n = ::send(static_cast<int>(handle_), p, static_cast<size_t>(chunk), MSG_NOSIGNAL);
#endif
    if (n <= 0) return false;
    p += n;
    size -= static_cast<size_t>(n);
  }
  return true;
}

long Socket::receive(void* data, size_t size) {
  const int chunk = static_cast<int>(std::min<size_t>(size, 1 << 20));
#if defined(_WIN32)
  return ::recv(static_cast<SOCKET>(handle_), static_cast<char*>(data), chunk, 0);
#else
  return ::recv(static_cast<int>(handle_), data, static_cast<size_t>(chunk), 0);
#endif
}

void Socket::setTimeout(int ms) {
#if defined(_WIN32)
  const DWORD t = static_cast<DWORD>(ms);
  setsockopt(static_cast<SOCKET>(handle_), SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&t), sizeof(t));
  setsockopt(static_cast<SOCKET>(handle_), SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&t), sizeof(t));
#else
  timeval t{ms / 1000, (ms % 1000) * 1000};
  setsockopt(static_cast<int>(handle_), SOL_SOCKET, SO_RCVTIMEO, &t, sizeof(t));
  setsockopt(static_cast<int>(handle_), SOL_SOCKET, SO_SNDTIMEO, &t, sizeof(t));
#endif
}

Socket listenTcp(uint16_t port, std::string& error) {
  init();
  Socket s(static_cast<intptr_t>(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)));
  if (!s.valid()) return error = "socket() failed", Socket();
  const int yes = 1;
  setsockopt(static_cast<decltype(::socket(0, 0, 0))>(s.handle()), SOL_SOCKET, SO_REUSEADDR,
             reinterpret_cast<const char*>(&yes), sizeof(yes));
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(port);
  if (::bind(static_cast<decltype(::socket(0, 0, 0))>(s.handle()), reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0)
    return error = "bind() failed: port " + std::to_string(port) + " in use?", Socket();
  if (::listen(static_cast<decltype(::socket(0, 0, 0))>(s.handle()), 16) != 0) return error = "listen() failed", Socket();
  return s;
}

Socket acceptClient(const Socket& server, std::string* peer) {
  sockaddr_in addr{};
  SocketLength len = sizeof(addr);
  const auto h = ::accept(static_cast<decltype(::socket(0, 0, 0))>(server.handle()), reinterpret_cast<sockaddr*>(&addr), &len);
  Socket s(static_cast<intptr_t>(h));
#if defined(_WIN32)
  if (h == INVALID_SOCKET) return Socket();
#else
  if (h < 0) return Socket();
#endif
  if (peer) {
    char buf[INET_ADDRSTRLEN] = {};
    inet_ntop(AF_INET, &addr.sin_addr, buf, sizeof(buf));
    *peer = buf;
  }
  return s;
}

Socket connectTcp(const std::string& host, uint16_t port, int timeoutMs, std::string& error) {
  init();
  addrinfo hints{}, *res = nullptr;
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) != 0 || !res)
    return error = "cannot resolve " + host, Socket();
  Socket s(static_cast<intptr_t>(::socket(res->ai_family, res->ai_socktype, res->ai_protocol)));
  if (s.valid()) s.setTimeout(timeoutMs);
  const bool ok = s.valid() && ::connect(static_cast<decltype(::socket(0, 0, 0))>(s.handle()), res->ai_addr,
                                         static_cast<SocketLength>(res->ai_addrlen)) == 0;
  freeaddrinfo(res);
  if (!ok) return error = "cannot connect to " + host + ":" + std::to_string(port), Socket();
  return s;
}

// ---- discovery -------------------------------------------------------------------------------
void serveDiscovery(uint16_t httpPort, const bool& stop) {
  init();
  Socket s(static_cast<intptr_t>(::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)));
  if (!s.valid()) return;
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(kDiscoveryPort);
  if (::bind(static_cast<decltype(::socket(0, 0, 0))>(s.handle()), reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) return;
  s.setTimeout(500);
  const std::string reply = "GSPLAT_SERVER " + std::to_string(httpPort);
  while (!stop) {
    char buf[64] = {};
    sockaddr_in from{};
    SocketLength len = sizeof(from);
    const auto n = ::recvfrom(static_cast<decltype(::socket(0, 0, 0))>(s.handle()), buf, sizeof(buf) - 1, 0,
                              reinterpret_cast<sockaddr*>(&from), &len);
    if (n > 0 && std::strncmp(buf, "GSPLAT_DISCOVER", 15) == 0)
      ::sendto(static_cast<decltype(::socket(0, 0, 0))>(s.handle()), reply.c_str(), static_cast<int>(reply.size()), 0,
               reinterpret_cast<sockaddr*>(&from), len);
  }
}

std::string discoverServer(int timeoutMs, uint16_t* httpPort) {
  init();
  Socket s(static_cast<intptr_t>(::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)));
  if (!s.valid()) return {};
  const int yes = 1;
  setsockopt(static_cast<decltype(::socket(0, 0, 0))>(s.handle()), SOL_SOCKET, SO_BROADCAST,
             reinterpret_cast<const char*>(&yes), sizeof(yes));
  s.setTimeout(timeoutMs);
  sockaddr_in to{};
  to.sin_family = AF_INET;
  to.sin_addr.s_addr = htonl(INADDR_BROADCAST);
  to.sin_port = htons(kDiscoveryPort);
  const char msg[] = "GSPLAT_DISCOVER";
  ::sendto(static_cast<decltype(::socket(0, 0, 0))>(s.handle()), msg, static_cast<int>(sizeof(msg) - 1), 0,
           reinterpret_cast<sockaddr*>(&to), sizeof(to));
  char buf[64] = {};
  sockaddr_in from{};
  SocketLength len = sizeof(from);
  const auto n = ::recvfrom(static_cast<decltype(::socket(0, 0, 0))>(s.handle()), buf, sizeof(buf) - 1, 0,
                            reinterpret_cast<sockaddr*>(&from), &len);
  if (n <= 0 || std::strncmp(buf, "GSPLAT_SERVER ", 14) != 0) return {};
  if (httpPort) *httpPort = static_cast<uint16_t>(std::atoi(buf + 14));
  char ip[INET_ADDRSTRLEN] = {};
  inet_ntop(AF_INET, &from.sin_addr, ip, sizeof(ip));
  return ip;
}

// ---- HTTP ------------------------------------------------------------------------------------
std::string urlDecode(const std::string& s) {
  std::string out;
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '%' && i + 2 < s.size() && std::isxdigit(static_cast<unsigned char>(s[i + 1])) &&
        std::isxdigit(static_cast<unsigned char>(s[i + 2]))) {
      out += static_cast<char>(std::stoi(s.substr(i + 1, 2), nullptr, 16));
      i += 2;
    } else {
      out += s[i] == '+' ? ' ' : s[i];
    }
  }
  return out;
}

std::string urlEncode(const std::string& s) {
  static const char* hex = "0123456789ABCDEF";
  std::string out;
  for (unsigned char c : s) {
    if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '/' || c == '~') {
      out += static_cast<char>(c);
    } else {
      out += '%';
      out += hex[c >> 4];
      out += hex[c & 15];
    }
  }
  return out;
}

std::string queryValue(const std::string& query, const std::string& key) {
  size_t pos = 0;
  while (pos <= query.size()) {
    const size_t amp = query.find('&', pos);
    const std::string pair = query.substr(pos, amp == std::string::npos ? std::string::npos : amp - pos);
    const size_t eq = pair.find('=');
    if (pair.substr(0, eq) == key) return eq == std::string::npos ? std::string() : urlDecode(pair.substr(eq + 1));
    if (amp == std::string::npos) break;
    pos = amp + 1;
  }
  return {};
}

bool readRequest(Socket& s, Request& r) {
  std::string data;
  char buf[4096];
  size_t end;
  while ((end = data.find("\r\n\r\n")) == std::string::npos) {
    const long n = s.receive(buf, sizeof(buf));
    if (n <= 0 || data.size() > 64 * 1024) return false;
    data.append(buf, static_cast<size_t>(n));
  }
  r.bodyStart = data.substr(end + 4);
  const std::string head = data.substr(0, end);
  size_t lineEnd = head.find("\r\n");
  const std::string line = head.substr(0, lineEnd);
  const size_t sp1 = line.find(' '), sp2 = line.rfind(' ');
  if (sp1 == std::string::npos || sp2 <= sp1) return false;
  r.method = line.substr(0, sp1);
  std::string target = line.substr(sp1 + 1, sp2 - sp1 - 1);
  const size_t q = target.find('?');
  r.query = q == std::string::npos ? std::string() : target.substr(q + 1);
  r.path = urlDecode(target.substr(0, q));
  while (lineEnd != std::string::npos) {
    const size_t next = head.find("\r\n", lineEnd + 2);
    const std::string h = head.substr(lineEnd + 2, next == std::string::npos ? std::string::npos : next - lineEnd - 2);
    const size_t colon = h.find(':');
    if (colon != std::string::npos) {
      std::string v = h.substr(colon + 1);
      v.erase(0, v.find_first_not_of(' '));
      r.headers[lower(h.substr(0, colon))] = v;
    }
    lineEnd = next;
  }
  const auto cl = r.headers.find("content-length");
  if (cl != r.headers.end()) r.contentLength = std::atoll(cl->second.c_str());
  return true;
}

bool sendResponse(Socket& s, int status, const std::string& contentType, const std::string& body) {
  std::string h = "HTTP/1.1 " + std::to_string(status) + " " + statusText(status) + "\r\nContent-Type: " + contentType +
                  "\r\nContent-Length: " + std::to_string(body.size()) +
                  "\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n";
  return s.sendAll(h) && s.sendAll(body);
}

bool sendFile(Socket& s, const std::string& utf8Path, const std::string& contentType) {
  std::ifstream f(pathFromUtf8(utf8Path), std::ios::binary | std::ios::ate);
  if (!f) return sendResponse(s, 404, "text/plain", "not found\n");
  const auto size = static_cast<long long>(f.tellg());
  f.seekg(0);
  std::string h = "HTTP/1.1 200 OK\r\nContent-Type: " + contentType + "\r\nContent-Length: " + std::to_string(size) +
                  "\r\nConnection: close\r\n\r\n";
  if (!s.sendAll(h)) return false;
  std::string buf(1 << 20, '\0');
  while (f) {
    f.read(buf.data(), static_cast<std::streamsize>(buf.size()));
    const auto n = f.gcount();
    if (n > 0 && !s.sendAll(buf.data(), static_cast<size_t>(n))) return false;
  }
  return true;
}

bool httpGet(const std::string& host, uint16_t port, const std::string& path, Response& out, std::string& error,
             const std::function<bool(const char*, size_t)>& onData, int timeoutMs) {
  out = {};
  Socket s = connectTcp(host, port, timeoutMs, error);
  if (!s.valid()) return false;
  // Encode the path only: the query string is sent as given (callers encode its values), otherwise
  // "/api/files?name=x" would become "/api/files%3Fname%3Dx" and miss the route.
  const size_t q = path.find('?');
  const std::string target = urlEncode(path.substr(0, q)) + (q == std::string::npos ? std::string() : path.substr(q));
  if (!s.sendAll("GET " + target + " HTTP/1.1\r\nHost: " + host + "\r\nConnection: close\r\n\r\n"))
    return error = "send failed", false;
  std::string head;
  char buf[1 << 16];
  size_t end;
  while ((end = head.find("\r\n\r\n")) == std::string::npos) {
    const long n = s.receive(buf, sizeof(buf));
    if (n <= 0) return error = "no response", false;
    head.append(buf, static_cast<size_t>(n));
  }
  std::string rest = head.substr(end + 4);
  head.resize(end);
  out.status = std::atoi(head.c_str() + head.find(' ') + 1);
  size_t pos = head.find("\r\n");
  while (pos != std::string::npos) {
    const size_t next = head.find("\r\n", pos + 2);
    const std::string h = head.substr(pos + 2, next == std::string::npos ? std::string::npos : next - pos - 2);
    const size_t colon = h.find(':');
    if (colon != std::string::npos) {
      std::string v = h.substr(colon + 1);
      v.erase(0, v.find_first_not_of(' '));
      out.headers[lower(h.substr(0, colon))] = v;
    }
    pos = next;
  }
  const long long expected =
      out.headers.count("content-length") ? std::atoll(out.headers["content-length"].c_str()) : -1;
  long long received = 0;
  auto deliver = [&](const char* p, size_t n) {
    received += static_cast<long long>(n);
    if (onData) return onData(p, n);
    out.body.append(p, n);
    return true;
  };
  if (!rest.empty() && !deliver(rest.data(), rest.size())) return error = "cancelled", false;
  while (expected < 0 || received < expected) {
    const long n = s.receive(buf, sizeof(buf));
    if (n == 0) break;
    if (n < 0) return error = "connection lost", false;
    if (!deliver(buf, static_cast<size_t>(n))) return error = "cancelled", false;
  }
  if (expected >= 0 && received != expected) return error = "incomplete download", false;
  return true;
}

}  // namespace gsv::net

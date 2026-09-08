#include "SonyProxy.h"

#include <WiFi.h>
#include <algorithm>
#include <lwip/sockets.h>
#include <lwip/netdb.h>
#include <mbedtls/base64.h>
#include <errno.h>
#include <cstring>
#include <memory>
#include <new>
#include <vector>

namespace {
constexpr uint16_t PROXY_PORT = 8081;
constexpr uint8_t MAX_SESSIONS = 4;
constexpr uint8_t LISTEN_BACKLOG = 8;
constexpr uint32_t SESSION_SLOT_WAIT_MS = 1500;
constexpr size_t MAX_HEADER_BYTES = 16384;
constexpr size_t MAX_CAPTURE_BYTES = 256;
constexpr size_t MAX_WS_BUFFER = 32768;
constexpr uint32_t IO_TIMEOUT_MS = 5000;

bool sendAll(int fd, const uint8_t *data, size_t len) {
  size_t sent = 0;
  while (sent < len) {
    int n = ::send(fd, data + sent, len - sent, 0);
    if (n <= 0) return false;
    sent += static_cast<size_t>(n);
  }
  return true;
}

bool sendAll(int fd, const String &s) {
  return sendAll(fd, reinterpret_cast<const uint8_t *>(s.c_str()), s.length());
}

void setTimeouts(int fd, uint32_t timeoutMs) {
  struct timeval tv {};
  tv.tv_sec = timeoutMs / 1000;
  tv.tv_usec = (timeoutMs % 1000) * 1000;
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

bool readHeaders(int fd, String &headers, std::vector<uint8_t> &leftover) {
  headers = "";
  leftover.clear();
  std::vector<uint8_t> raw;
  raw.reserve(2048);
  uint8_t buf[1024];
  while (raw.size() < MAX_HEADER_BYTES) {
    int n = recv(fd, buf, sizeof(buf), 0);
    if (n <= 0) return false;
    raw.insert(raw.end(), buf, buf + n);
    if (raw.size() >= 4) {
      for (size_t i = 3; i < raw.size(); ++i) {
        if (raw[i-3] == '\r' && raw[i-2] == '\n' && raw[i-1] == '\r' && raw[i] == '\n') {
          size_t headerLen = i + 1;
          headers.reserve(headerLen);
          for (size_t j = 0; j < headerLen; ++j) headers += static_cast<char>(raw[j]);
          leftover.assign(raw.begin() + headerLen, raw.end());
          return true;
        }
      }
    }
  }
  return false;
}

String headerValue(const String &headers, const String &wantedLower) {
  int start = headers.indexOf("\r\n");
  if (start < 0) return String();
  start += 2;
  while (start < headers.length()) {
    int end = headers.indexOf("\r\n", start);
    if (end < 0) end = headers.length();
    String line = headers.substring(start, end);
    int colon = line.indexOf(':');
    if (colon > 0) {
      String name = line.substring(0, colon);
      name.trim(); name.toLowerCase();
      if (name == wantedLower) {
        String value = line.substring(colon + 1);
        value.trim();
        return value;
      }
    }
    start = end + 2;
  }
  return String();
}

bool hasToken(String value, const char *token) {
  value.toLowerCase();
  String t(token); t.toLowerCase();
  int start = 0;
  while (start < value.length()) {
    int comma = value.indexOf(',', start);
    if (comma < 0) comma = value.length();
    String part = value.substring(start, comma);
    part.trim();
    if (part == t) return true;
    start = comma + 1;
  }
  return false;
}

bool parseRequestLine(const String &headers, String &method, String &path, String &version) {
  int end = headers.indexOf("\r\n");
  if (end < 0) return false;
  String line = headers.substring(0, end);
  int a = line.indexOf(' ');
  int b = a < 0 ? -1 : line.indexOf(' ', a + 1);
  if (a <= 0 || b <= a + 1) return false;
  method = line.substring(0, a);
  path = line.substring(a + 1, b);
  version = line.substring(b + 1);
  return method.length() && path.startsWith("/") && version.startsWith("HTTP/");
}

size_t parseContentLength(const String &headers) {
  String v = headerValue(headers, "content-length");
  if (!v.length()) return 0;
  char *end = nullptr;
  unsigned long n = strtoul(v.c_str(), &end, 10);
  return (end && *end == '\0') ? static_cast<size_t>(n) : 0;
}

String rewriteRequestHeaders(const String &headers, const String &cameraHost,
                             const String &auth, bool websocket, String &clientHost) {
  String method, path, version;
  if (!parseRequestLine(headers, method, path, version)) return String();
  clientHost = headerValue(headers, "host");

  String out = method + " " + path + " " + version + "\r\n";
  int start = headers.indexOf("\r\n") + 2;
  while (start >= 2 && start < headers.length()) {
    int end = headers.indexOf("\r\n", start);
    if (end < 0) break;
    if (end == start) break;
    String line = headers.substring(start, end);
    int colon = line.indexOf(':');
    if (colon > 0) {
      String name = line.substring(0, colon);
      String lower = name; lower.trim(); lower.toLowerCase();
      if (lower != "host" && lower != "authorization" && lower != "origin" &&
          lower != "referer" && lower != "connection" && lower != "proxy-connection") {
        out += line + "\r\n";
      }
    }
    start = end + 2;
  }
  out += "Host: " + cameraHost + "\r\n";
  if (auth.length()) out += "Authorization: Basic " + auth + "\r\n";
  if (websocket) {
    out += "Origin: http://" + cameraHost + "\r\n";
    out += "Connection: Upgrade\r\n";
  } else {
    out += "Connection: close\r\n";
  }
  out += "\r\n";
  return out;
}

String rewriteResponseHeaders(const String &headers, const String &cameraHost,
                              const String &clientHost, bool websocket) {
  int firstEnd = headers.indexOf("\r\n");
  if (firstEnd < 0) return headers;
  String out = headers.substring(0, firstEnd) + "\r\n";
  int start = firstEnd + 2;
  while (start < headers.length()) {
    int end = headers.indexOf("\r\n", start);
    if (end < 0) break;
    if (end == start) break;
    String line = headers.substring(start, end);
    int colon = line.indexOf(':');
    if (colon > 0) {
      String name = line.substring(0, colon);
      String lower = name; lower.trim(); lower.toLowerCase();
      if (lower == "connection") {
        // Replaced below so ordinary proxied requests do not leave dangling keep-alive sockets.
      } else if (lower == "location" && clientHost.length()) {
        String value = line.substring(colon + 1); value.trim();
        value.replace("http://" + cameraHost, "http://" + clientHost);
        out += name + ": " + value + "\r\n";
      } else {
        out += line + "\r\n";
      }
    }
    start = end + 2;
  }
  out += websocket ? "Connection: Upgrade\r\n\r\n" : "Connection: close\r\n\r\n";
  return out;
}

int parseStatus(const String &headers) {
  int end = headers.indexOf("\r\n");
  String line = end >= 0 ? headers.substring(0, end) : headers;
  int sp = line.indexOf(' ');
  if (sp < 0 || line.length() < sp + 4) return 0;
  return line.substring(sp + 1, sp + 4).toInt();
}

bool readMsgpackUInt(const std::vector<uint8_t> &buf, size_t &p, uint64_t &value) {
  if (p >= buf.size()) return false;
  uint8_t c = buf[p++];
  if (c <= 0x7F) { value = c; return true; }
  size_t n = 0;
  if (c == 0xCC) n = 1; else if (c == 0xCD) n = 2; else if (c == 0xCE) n = 4; else if (c == 0xCF) n = 8; else return false;
  if (p + n > buf.size()) return false;
  uint64_t v = 0;
  for (size_t i = 0; i < n; ++i) v = (v << 8) | buf[p++];
  value = v;
  return true;
}

bool readMsgpackArrayLen(const std::vector<uint8_t> &buf, size_t &p, uint32_t &count) {
  if (p >= buf.size()) return false;
  uint8_t c = buf[p++];
  if ((c & 0xF0) == 0x90) { count = c & 0x0F; return true; }
  if (c == 0xDC) {
    if (p + 2 > buf.size()) return false;
    count = (static_cast<uint32_t>(buf[p]) << 8) | buf[p+1]; p += 2; return true;
  }
  return false;
}

bool readMsgpackString(const std::vector<uint8_t> &buf, size_t &p, String &out) {
  if (p >= buf.size()) return false;
  uint8_t c = buf[p++];
  size_t n = 0;
  if ((c & 0xE0) == 0xA0) n = c & 0x1F;
  else if (c == 0xD9) { if (p >= buf.size()) return false; n = buf[p++]; }
  else if (c == 0xDA) { if (p + 2 > buf.size()) return false; n = (buf[p] << 8) | buf[p+1]; p += 2; }
  else return false;
  if (p + n > buf.size()) return false;
  out = ""; out.reserve(n);
  for (size_t i = 0; i < n; ++i) out += static_cast<char>(buf[p+i]);
  p += n;
  return true;
}

String rpcSummary(const std::vector<uint8_t> &payload) {
  size_t p = 0; uint32_t count = 0; uint64_t type = 0;
  if (!readMsgpackArrayLen(payload, p, count) || count < 2 || !readMsgpackUInt(payload, p, type)) return String();
  if (type == 0 && count >= 4) {
    uint64_t id = 0; String method;
    if (!readMsgpackUInt(payload, p, id) || !readMsgpackString(payload, p, method)) return String();
    String out = "RPC request #" + String(static_cast<unsigned long>(id)) + " " + method;
    // Sony button names are strings inside params. Scan printable runs so a capture
    // of Button.SendKeys is useful even without a full MessagePack pretty-printer.
    if (method == "Button.SendKeys") {
      String printable;
      for (size_t i = p; i < payload.size(); ++i) {
        char ch = static_cast<char>(payload[i]);
        if (ch >= 32 && ch <= 126) printable += ch;
        else if (printable.length() && printable.length() < 3) printable = "";
      }
      if (printable.length()) out += " [" + printable.substring(0, 72) + "]";
    }
    return out;
  }
  if (type == 1 && count >= 4) {
    uint64_t id = 0; if (!readMsgpackUInt(payload, p, id)) return "RPC response";
    return "RPC response #" + String(static_cast<unsigned long>(id));
  }
  if (type == 2 && count >= 3) {
    String name;
    if (readMsgpackString(payload, p, name)) return "RPC notify " + name;
    return "RPC notification";
  }
  return "MessagePack envelope type " + String(static_cast<unsigned long>(type));
}
}

void SonyProxy::begin() {
  xTaskCreatePinnedToCore(listenerTaskThunk, "sony-proxy-listen", 4096, this, 1,
                          &listenerTask_, 0);
}

void SonyProxy::listenerTaskThunk(void *arg) {
  static_cast<SonyProxy *>(arg)->listenerTask();
}

void SonyProxy::sessionTaskThunk(void *arg) {
  std::unique_ptr<SessionContext> ctx(static_cast<SessionContext *>(arg));
  SonyProxy *self = ctx->self;
  int fd = ctx->clientFd;
  self->sessionTask(fd);
  self->releaseSession();
  vTaskDelete(nullptr);
}

bool SonyProxy::claimSession() {
  bool ok = false;
  portENTER_CRITICAL(&sessionMux_);
  if (activeSessions_ < MAX_SESSIONS) { ++activeSessions_; ok = true; }
  portEXIT_CRITICAL(&sessionMux_);
  return ok;
}

void SonyProxy::releaseSession() {
  portENTER_CRITICAL(&sessionMux_);
  if (activeSessions_) --activeSessions_;
  portEXIT_CRITICAL(&sessionMux_);
}

void SonyProxy::listenerTask() {
  int listenFd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (listenFd < 0) {
    Serial.printf("[sony-proxy] socket failed: %s\n", strerror(errno));
    vTaskDelete(nullptr); return;
  }
  int yes = 1;
  setsockopt(listenFd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
  struct sockaddr_in addr {};
  addr.sin_family = AF_INET; addr.sin_port = htons(PROXY_PORT); addr.sin_addr.s_addr = htonl(INADDR_ANY);
  if (bind(listenFd, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) != 0 ||
      listen(listenFd, LISTEN_BACKLOG) != 0) {
    Serial.printf("[sony-proxy] listen failed: %s\n", strerror(errno));
    close(listenFd); vTaskDelete(nullptr); return;
  }
  Serial.printf("[sony-proxy] native Sony proxy listening on port %u\n", PROXY_PORT);
  for (;;) {
    int clientFd = accept(listenFd, nullptr, nullptr);
    if (clientFd < 0) { vTaskDelay(pdMS_TO_TICKS(50)); continue; }
    setTimeouts(clientFd, IO_TIMEOUT_MS);
    // Sony's native page loads many scripts/images in parallel. Browsers commonly
    // open more simultaneous connections than our four worker slots. Returning an
    // immediate 503 can drop a critical script, causing InitData() to throw before
    // the page ever calls client.connect(). Keep the memory cap at four workers,
    // but briefly queue accepted requests until a slot becomes free.
    bool claimed = claimSession();
    const uint32_t waitStarted = millis();
    while (!claimed && millis() - waitStarted < SESSION_SLOT_WAIT_MS) {
      vTaskDelay(pdMS_TO_TICKS(10));
      claimed = claimSession();
    }
    if (!claimed) {
      const char busy[] = "HTTP/1.1 503 Busy\r\nConnection: close\r\nContent-Length: 0\r\n\r\n";
      sendAll(clientFd, reinterpret_cast<const uint8_t *>(busy), sizeof(busy)-1);
      close(clientFd); continue;
    }
    SessionContext *ctx = new (std::nothrow) SessionContext{this, clientFd};
    TaskHandle_t task = nullptr;
    if (!ctx || xTaskCreatePinnedToCore(sessionTaskThunk, "sony-proxy", 6144, ctx, 1, &task, 0) != pdPASS) {
      delete ctx; close(clientFd); releaseSession();
    }
  }
}

String SonyProxy::basicAuthorization() const {
  String plain = cfg_.cameraUsername + ":" + cfg_.cameraPassword;
  size_t required = 0;
  mbedtls_base64_encode(nullptr, 0, &required,
                        reinterpret_cast<const unsigned char *>(plain.c_str()), plain.length());
  if (!required) return String();
  std::unique_ptr<unsigned char[]> out(new (std::nothrow) unsigned char[required + 1]);
  if (!out) return String();
  size_t written = 0;
  if (mbedtls_base64_encode(out.get(), required, &written,
                            reinterpret_cast<const unsigned char *>(plain.c_str()), plain.length()) != 0) return String();
  out[written] = 0;
  return String(reinterpret_cast<char *>(out.get()));
}

int SonyProxy::connectCamera(uint32_t timeoutMs, String &error) const {
  if (WiFi.status() != WL_CONNECTED) { error = "camera Wi-Fi is disconnected"; return -1; }
  struct sockaddr_in remote {};
  remote.sin_family = AF_INET; remote.sin_port = htons(80);
  if (inet_pton(AF_INET, cfg_.cameraHost.c_str(), &remote.sin_addr) != 1) {
    struct addrinfo hints {}; hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM;
    struct addrinfo *res = nullptr;
    if (getaddrinfo(cfg_.cameraHost.c_str(), "80", &hints, &res) != 0 || !res) {
      if (res) freeaddrinfo(res);
      error = "could not resolve camera host";
      return -1;
    }
    remote = *reinterpret_cast<struct sockaddr_in *>(res->ai_addr); freeaddrinfo(res);
  }
  int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (fd < 0) { error = String("socket failed: ") + strerror(errno); return -1; }
  setTimeouts(fd, timeoutMs);
  struct sockaddr_in local {};
  local.sin_family = AF_INET; local.sin_port = 0;
  local.sin_addr.s_addr = inet_addr(WiFi.localIP().toString().c_str());
  if (bind(fd, reinterpret_cast<struct sockaddr *>(&local), sizeof(local)) != 0) {
    error = String("Wi-Fi source bind failed: ") + strerror(errno); close(fd); return -1;
  }
  if (connect(fd, reinterpret_cast<struct sockaddr *>(&remote), sizeof(remote)) != 0) {
    error = String("camera connect failed: ") + strerror(errno); close(fd); return -1;
  }
  return fd;
}

void SonyProxy::sessionTask(int clientFd) {
  String requestHeaders; std::vector<uint8_t> requestLeft;
  if (!readHeaders(clientFd, requestHeaders, requestLeft)) { close(clientFd); return; }
  String method, path, version;
  if (!parseRequestLine(requestHeaders, method, path, version)) { close(clientFd); return; }
  bool websocket = hasToken(headerValue(requestHeaders, "upgrade"), "websocket");
  String clientHost;
  String upstreamHeaders = rewriteRequestHeaders(requestHeaders, cfg_.cameraHost,
                                                  basicAuthorization(), websocket, clientHost);
  if (!upstreamHeaders.length()) { close(clientFd); return; }

  String error;
  int cameraFd = connectCamera(IO_TIMEOUT_MS, error);
  if (cameraFd < 0) {
    String body = "Sony camera proxy unavailable: " + error;
    String response = "HTTP/1.1 503 Service Unavailable\r\nContent-Type: text/plain\r\nConnection: close\r\nContent-Length: " + String(body.length()) + "\r\n\r\n" + body;
    sendAll(clientFd, response); close(clientFd); return;
  }
  if (!sendAll(cameraFd, upstreamHeaders)) { close(cameraFd); close(clientFd); return; }

  const size_t bodyLen = parseContentLength(requestHeaders);
  size_t forwardedBody = 0;
  if (!requestLeft.empty()) {
    size_t n = bodyLen ? std::min(bodyLen, requestLeft.size()) : requestLeft.size();
    if (n && !sendAll(cameraFd, requestLeft.data(), n)) { close(cameraFd); close(clientFd); return; }
    forwardedBody += n;
  }
  std::vector<uint8_t> capturedBody;
  if (captureEnabled_ && bodyLen) {
    capturedBody.reserve(std::min<size_t>(bodyLen, MAX_CAPTURE_BYTES));
    size_t n = std::min<size_t>(forwardedBody, MAX_CAPTURE_BYTES);
    capturedBody.insert(capturedBody.end(), requestLeft.begin(), requestLeft.begin() + std::min(n, requestLeft.size()));
  }
  uint8_t buf[2048];
  while (forwardedBody < bodyLen) {
    int n = recv(clientFd, buf, std::min<size_t>(sizeof(buf), bodyLen - forwardedBody), 0);
    if (n <= 0) { close(cameraFd); close(clientFd); return; }
    if (!sendAll(cameraFd, buf, static_cast<size_t>(n))) { close(cameraFd); close(clientFd); return; }
    if (captureEnabled_ && capturedBody.size() < MAX_CAPTURE_BYTES) {
      size_t add = std::min<size_t>(static_cast<size_t>(n), MAX_CAPTURE_BYTES - capturedBody.size());
      capturedBody.insert(capturedBody.end(), buf, buf + add);
    }
    forwardedBody += static_cast<size_t>(n);
  }
  captureHttpRequest(method, path, capturedBody.data(), capturedBody.size());

  String responseHeaders; std::vector<uint8_t> responseLeft;
  if (!readHeaders(cameraFd, responseHeaders, responseLeft)) { close(cameraFd); close(clientFd); return; }
  int status = parseStatus(responseHeaders);
  bool upgraded = websocket && status == 101;
  String downstreamHeaders = rewriteResponseHeaders(responseHeaders, cfg_.cameraHost, clientHost, upgraded);
  if (!sendAll(clientFd, downstreamHeaders)) { close(cameraFd); close(clientFd); return; }
  if (!responseLeft.empty() && !sendAll(clientFd, responseLeft.data(), responseLeft.size())) {
    close(cameraFd); close(clientFd); return;
  }

  if (!upgraded) {
    // Ordinary native-web requests are forced close upstream; stream until Sony closes.
    for (;;) {
      int n = recv(cameraFd, buf, sizeof(buf), 0);
      if (n <= 0) break;
      if (!sendAll(clientFd, buf, static_cast<size_t>(n))) break;
    }
    close(cameraFd); close(clientFd); return;
  }

  Serial.printf("[sony-proxy] WebSocket /linear tunnel established\n");
  WsSniffer browserSniffer{this, true, {}};
  WsSniffer cameraSniffer{this, false, {}};
  if (!responseLeft.empty()) cameraSniffer.feed(responseLeft.data(), responseLeft.size());

  // Raw bidirectional tunnel. Sony's own JS remains responsible for the protocol;
  // the bridge only observes complete WebSocket frames when capture is enabled.
  for (;;) {
    fd_set reads; FD_ZERO(&reads); FD_SET(clientFd, &reads); FD_SET(cameraFd, &reads);
    int maxfd = clientFd > cameraFd ? clientFd : cameraFd;
    struct timeval tv {1, 0};
    int ready = select(maxfd + 1, &reads, nullptr, nullptr, &tv);
    if (ready < 0) break;
    if (ready == 0) continue;
    if (FD_ISSET(clientFd, &reads)) {
      int n = recv(clientFd, buf, sizeof(buf), 0);
      if (n <= 0) break;
      browserSniffer.feed(buf, static_cast<size_t>(n));
      if (!sendAll(cameraFd, buf, static_cast<size_t>(n))) break;
    }
    if (FD_ISSET(cameraFd, &reads)) {
      int n = recv(cameraFd, buf, sizeof(buf), 0);
      if (n <= 0) break;
      cameraSniffer.feed(buf, static_cast<size_t>(n));
      if (!sendAll(clientFd, buf, static_cast<size_t>(n))) break;
    }
  }
  close(cameraFd); close(clientFd);
  Serial.println("[sony-proxy] WebSocket /linear tunnel closed");
}

void SonyProxy::WsSniffer::feed(const uint8_t *data, size_t len) {
  if (!owner || !data || !len) return;
  if (!owner->captureEnabled_) return;
  if (buffer.size() + len > MAX_WS_BUFFER) { buffer.clear(); return; }
  buffer.insert(buffer.end(), data, data + len);
  for (;;) {
    if (buffer.size() < 2) return;
    uint8_t b0 = buffer[0], b1 = buffer[1];
    bool masked = (b1 & 0x80) != 0;
    uint64_t payloadLen = b1 & 0x7F;
    size_t pos = 2;
    if (payloadLen == 126) {
      if (buffer.size() < 4) return;
      payloadLen = (static_cast<uint64_t>(buffer[2]) << 8) | buffer[3]; pos = 4;
    } else if (payloadLen == 127) {
      if (buffer.size() < 10) return;
      payloadLen = 0;
      for (int i = 2; i < 10; ++i) payloadLen = (payloadLen << 8) | buffer[i];
      pos = 10;
    }
    if (payloadLen > MAX_WS_BUFFER) { buffer.clear(); return; }
    uint8_t mask[4] = {0,0,0,0};
    if (masked) {
      if (buffer.size() < pos + 4) return;
      memcpy(mask, buffer.data() + pos, 4); pos += 4;
    }
    if (buffer.size() < pos + payloadLen) return;
    uint8_t opcode = b0 & 0x0F;
    std::vector<uint8_t> payload(static_cast<size_t>(payloadLen));
    for (size_t i = 0; i < payload.size(); ++i) {
      uint8_t v = buffer[pos + i];
      payload[i] = masked ? static_cast<uint8_t>(v ^ mask[i & 3]) : v;
    }
    owner->captureWsPayload(fromBrowser, opcode, payload);
    buffer.erase(buffer.begin(), buffer.begin() + pos + static_cast<size_t>(payloadLen));
  }
}

void SonyProxy::captureWsPayload(bool fromBrowser, uint8_t opcode, const std::vector<uint8_t> &payload) {
  if (!captureEnabled_) return;
  // Keep the capture focused on commands sent by Sony's browser UI. Camera
  // notifications are high-volume and would quickly overwrite the useful press.
  if (!fromBrowser) return;
  if (opcode != 0x1 && opcode != 0x2) return;
  String summary = opcode == 0x2 ? rpcSummary(payload) : String("WebSocket text frame");
  if (!summary.length()) summary = "WebSocket binary frame";
  // Sony's rmt page repeatedly writes P.Menu.pmw-f5x.Menu.Opened=false as
  // housekeeping. The capture ring is intentionally tiny, so discard that
  // exact low-value write before it can overwrite operator interactions.
  static const char MENU_OPENED[] = "P.Menu.pmw-f5x.Menu.Opened";
  if (std::search(payload.begin(), payload.end(), MENU_OPENED, MENU_OPENED + strlen(MENU_OPENED)) != payload.end()) return;
  captureEvent("browser", "websocket", summary, payload.data(), payload.size());
}

void SonyProxy::captureHttpRequest(const String &method, const String &path,
                                   const uint8_t *body, size_t bodyLen) {
  if (!captureEnabled_) return;
  String summary = method + " " + path;
  captureEvent("browser", "http", summary, body, bodyLen);
}

void SonyProxy::captureEvent(const char *direction, const char *kind, const String &summary,
                             const uint8_t *payload, size_t payloadLen) {
  if (!captureEnabled_) return;
  CaptureEntry entry;
  entry.ms = millis();
  snprintf(entry.direction, sizeof(entry.direction), "%s", direction ? direction : "");
  snprintf(entry.kind, sizeof(entry.kind), "%s", kind ? kind : "");
  snprintf(entry.summary, sizeof(entry.summary), "%s", summary.c_str());
  entry.payloadBytes = static_cast<uint16_t>(std::min<size_t>(payloadLen, 65535));
  entry.truncated = payloadLen > MAX_CAPTURE_BYTES;
  size_t n = std::min<size_t>(payloadLen, MAX_CAPTURE_BYTES);
  static const char HEX_DIGITS[] = "0123456789abcdef";
  for (size_t i = 0; i < n; ++i) {
    entry.hex[i*2] = HEX_DIGITS[(payload[i] >> 4) & 0xF];
    entry.hex[i*2+1] = HEX_DIGITS[payload[i] & 0xF];
  }
  entry.hex[n*2] = 0;

  portENTER_CRITICAL(&captureMux_);
  entry.seq = ++captureSeq_;
  capture_[captureHead_] = entry;
  captureHead_ = (captureHead_ + 1) % capture_.size();
  if (captureCount_ < capture_.size()) ++captureCount_;
  portEXIT_CRITICAL(&captureMux_);
}

void SonyProxy::clearCapture() {
  portENTER_CRITICAL(&captureMux_);
  captureHead_ = 0; captureCount_ = 0; captureSeq_ = 0;
  for (auto &e : capture_) e = CaptureEntry{};
  portEXIT_CRITICAL(&captureMux_);
}

void SonyProxy::startCapture() {
  clearCapture();
  captureEnabled_ = true;
  Serial.println("[sony-proxy] capture started (RAM-only, browser -> FS7 requests)");
}

void SonyProxy::stopCapture() {
  captureEnabled_ = false;
  Serial.println("[sony-proxy] capture stopped");
}

String SonyProxy::captureJson() {
  String out;
  out.reserve(12288);
  out = "{\"enabled\":";
  out += captureEnabled_ ? "true" : "false";
  out += ",\"mode\":\"ram-only browser-to-camera\",\"entries\":[";

  uint8_t count, head;
  portENTER_CRITICAL(&captureMux_);
  count = captureCount_; head = captureHead_;
  portEXIT_CRITICAL(&captureMux_);
  uint8_t start = (head + capture_.size() - count) % capture_.size();
  for (uint8_t i = 0; i < count; ++i) {
    CaptureEntry e;
    portENTER_CRITICAL(&captureMux_);
    e = capture_[(start + i) % capture_.size()];
    portEXIT_CRITICAL(&captureMux_);
    if (i) out += ',';
    out += "{\"seq\":" + String(e.seq) + ",\"ms\":" + String(e.ms);
    out += ",\"direction\":\"" + String(e.direction) + "\",\"kind\":\"" + String(e.kind) + "\"";
    String s(e.summary); s.replace("\\", "\\\\"); s.replace("\"", "\\\""); s.replace("\r", "\\r"); s.replace("\n", "\\n");
    out += ",\"summary\":\"" + s + "\",\"payloadBytes\":" + String(e.payloadBytes);
    out += ",\"truncated\":"; out += e.truncated ? "true" : "false";
    out += ",\"hex\":\"" + String(e.hex) + "\"}";
  }
  out += "]}";
  return out;
}

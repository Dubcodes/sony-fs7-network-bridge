#include "SonyLinear.h"

#include <WiFi.h>
#include <lwip/sockets.h>
#include <lwip/netdb.h>
#include <mbedtls/base64.h>
#include <SHA1Builder.h>
#include <esp_system.h>
#include <errno.h>
#include <algorithm>
#include <cstring>
#include <memory>
#include <new>

namespace {
constexpr size_t MAX_HTTP_HEADERS = 8192;
constexpr size_t MAX_WS_PAYLOAD = 16384;
constexpr char WS_GUID[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

bool sendAll(int fd, const uint8_t *data, size_t length, String &error) {
  size_t sent = 0;
  while (sent < length) {
    int n = ::send(fd, data + sent, length - sent, 0);
    if (n <= 0) {
      error = String("send() failed: ") + strerror(errno);
      return false;
    }
    sent += static_cast<size_t>(n);
  }
  return true;
}

bool recvExact(int fd, uint8_t *out, size_t length, String &error) {
  size_t got = 0;
  while (got < length) {
    int n = recv(fd, out + got, length - got, 0);
    if (n == 0) {
      error = "Camera closed the WebSocket connection";
      return false;
    }
    if (n < 0) {
      error = (errno == EWOULDBLOCK || errno == EAGAIN)
          ? "Timed out waiting for WebSocket data"
          : String("recv() failed: ") + strerror(errno);
      return false;
    }
    got += static_cast<size_t>(n);
  }
  return true;
}

String base64Bytes(const uint8_t *data, size_t length) {
  size_t required = 0;
  mbedtls_base64_encode(nullptr, 0, &required, data, length);
  if (!required) return String();
  std::unique_ptr<unsigned char[]> out(new (std::nothrow) unsigned char[required + 1]);
  if (!out) return String();
  size_t written = 0;
  if (mbedtls_base64_encode(out.get(), required, &written, data, length) != 0) return String();
  out[written] = 0;
  return String(reinterpret_cast<char *>(out.get()));
}

bool readHttpHeaders(int fd, String &headers, String &leftover, String &error) {
  // Read through the header terminator without over-reading into a subsequent
  // WebSocket frame. The Sony server may respond very quickly after upgrade.
  String raw;
  raw.reserve(2048);
  while (!raw.endsWith("\r\n\r\n")) {
    char c = 0;
    int n = recv(fd, &c, 1, 0);
    if (n == 0) {
      error = "Connection closed before complete HTTP headers";
      return false;
    }
    if (n < 0) {
      error = (errno == EWOULDBLOCK || errno == EAGAIN)
          ? "Timed out before complete HTTP headers"
          : String("recv() failed: ") + strerror(errno);
      return false;
    }
    raw += c;
    if (raw.length() > MAX_HTTP_HEADERS) {
      error = "HTTP response headers exceed limit";
      return false;
    }
  }
  headers = raw.substring(0, raw.length() - 4);
  leftover = "";
  return true;
}

int parseHttpStatus(const String &headers) {
  int end = headers.indexOf("\r\n");
  String line = end >= 0 ? headers.substring(0, end) : headers;
  int first = line.indexOf(' ');
  if (!line.startsWith("HTTP/") || first < 0 || line.length() < first + 4) return 0;
  for (int i = 1; i <= 3; ++i) {
    if (!isdigit(static_cast<unsigned char>(line[first + i]))) return 0;
  }
  return line.substring(first + 1, first + 4).toInt();
}

bool headerValue(const String &headers, const String &wantedLower, String &value) {
  int start = headers.indexOf("\r\n");
  start = start < 0 ? headers.length() : start + 2;
  while (start < headers.length()) {
    int end = headers.indexOf("\r\n", start);
    if (end < 0) end = headers.length();
    String line = headers.substring(start, end);
    int colon = line.indexOf(':');
    if (colon > 0) {
      String name = line.substring(0, colon);
      name.trim();
      name.toLowerCase();
      if (name == wantedLower) {
        value = line.substring(colon + 1);
        value.trim();
        return true;
      }
    }
    start = end + 2;
  }
  return false;
}

void appendU16(std::vector<uint8_t> &out, uint16_t v) {
  out.push_back(static_cast<uint8_t>(v >> 8));
  out.push_back(static_cast<uint8_t>(v));
}

void appendU32(std::vector<uint8_t> &out, uint32_t v) {
  out.push_back(static_cast<uint8_t>(v >> 24));
  out.push_back(static_cast<uint8_t>(v >> 16));
  out.push_back(static_cast<uint8_t>(v >> 8));
  out.push_back(static_cast<uint8_t>(v));
}

void appendU64(std::vector<uint8_t> &out, uint64_t v) {
  for (int shift = 56; shift >= 0; shift -= 8) out.push_back(static_cast<uint8_t>(v >> shift));
}

bool encodeString(std::vector<uint8_t> &out, const char *s, size_t len) {
  if (len <= 31) out.push_back(static_cast<uint8_t>(0xA0 | len));
  else if (len <= 0xFF) { out.push_back(0xD9); out.push_back(static_cast<uint8_t>(len)); }
  else if (len <= 0xFFFF) { out.push_back(0xDA); appendU16(out, static_cast<uint16_t>(len)); }
  else return false;
  out.insert(out.end(), reinterpret_cast<const uint8_t *>(s), reinterpret_cast<const uint8_t *>(s) + len);
  return true;
}

bool encodeVariant(std::vector<uint8_t> &out, JsonVariantConst v, int depth) {
  if (depth > 10) return false;
  if (v.isNull()) { out.push_back(0xC0); return true; }
  if (v.is<bool>()) { out.push_back(v.as<bool>() ? 0xC3 : 0xC2); return true; }
  if (v.is<const char *>()) {
    const char *s = v.as<const char *>();
    return encodeString(out, s ? s : "", s ? strlen(s) : 0);
  }
  if (v.is<int64_t>()) {
    int64_t n = v.as<int64_t>();
    if (n >= 0 && n <= 127) out.push_back(static_cast<uint8_t>(n));
    else if (n >= -32 && n < 0) out.push_back(static_cast<uint8_t>(n));
    else if (n >= INT8_MIN && n <= INT8_MAX) { out.push_back(0xD0); out.push_back(static_cast<uint8_t>(n)); }
    else if (n >= INT16_MIN && n <= INT16_MAX) { out.push_back(0xD1); appendU16(out, static_cast<uint16_t>(n)); }
    else if (n >= INT32_MIN && n <= INT32_MAX) { out.push_back(0xD2); appendU32(out, static_cast<uint32_t>(n)); }
    else { out.push_back(0xD3); appendU64(out, static_cast<uint64_t>(n)); }
    return true;
  }
  if (v.is<uint64_t>()) {
    uint64_t n = v.as<uint64_t>();
    if (n <= 127) out.push_back(static_cast<uint8_t>(n));
    else if (n <= UINT8_MAX) { out.push_back(0xCC); out.push_back(static_cast<uint8_t>(n)); }
    else if (n <= UINT16_MAX) { out.push_back(0xCD); appendU16(out, static_cast<uint16_t>(n)); }
    else if (n <= UINT32_MAX) { out.push_back(0xCE); appendU32(out, static_cast<uint32_t>(n)); }
    else { out.push_back(0xCF); appendU64(out, n); }
    return true;
  }
  if (v.is<double>()) {
    union { double d; uint64_t u; } bits {v.as<double>()};
    out.push_back(0xCB);
    appendU64(out, bits.u);
    return true;
  }
  if (v.is<JsonArrayConst>()) {
    JsonArrayConst a = v.as<JsonArrayConst>();
    size_t n = a.size();
    if (n <= 15) out.push_back(static_cast<uint8_t>(0x90 | n));
    else if (n <= 0xFFFF) { out.push_back(0xDC); appendU16(out, static_cast<uint16_t>(n)); }
    else return false;
    for (JsonVariantConst item : a) if (!encodeVariant(out, item, depth + 1)) return false;
    return true;
  }
  if (v.is<JsonObjectConst>()) {
    JsonObjectConst o = v.as<JsonObjectConst>();
    size_t n = o.size();
    if (n <= 15) out.push_back(static_cast<uint8_t>(0x80 | n));
    else if (n <= 0xFFFF) { out.push_back(0xDE); appendU16(out, static_cast<uint16_t>(n)); }
    else return false;
    for (JsonPairConst kv : o) {
      const char *key = kv.key().c_str();
      if (!encodeString(out, key, strlen(key)) || !encodeVariant(out, kv.value(), depth + 1)) return false;
    }
    return true;
  }
  return false;
}

bool encodeRpc(uint32_t id, const String &method, const String &paramsJson,
               std::vector<uint8_t> &out, String &error) {
  if (!method.length() || method.length() > 128) { error = "Invalid Sony RPC method"; return false; }
  if (paramsJson.length() > 4096) { error = "Sony RPC params exceed limit"; return false; }
  JsonDocument paramsDoc;
  if (deserializeJson(paramsDoc, paramsJson)) { error = "Sony RPC params must be valid JSON"; return false; }
  if (!paramsDoc.as<JsonVariantConst>().is<JsonArrayConst>()) { error = "Sony RPC params must be a JSON array"; return false; }

  out.clear();
  out.reserve(method.length() + paramsJson.length() + 32);
  out.push_back(0x94);           // [type, id, method, params]
  out.push_back(0x00);           // type=0 request
  if (id <= 127) out.push_back(static_cast<uint8_t>(id));
  else if (id <= UINT16_MAX) { out.push_back(0xCD); appendU16(out, static_cast<uint16_t>(id)); }
  else { out.push_back(0xCE); appendU32(out, id); }
  if (!encodeString(out, method.c_str(), method.length())) { error = "Sony RPC method too long"; return false; }
  if (!encodeVariant(out, paramsDoc.as<JsonVariantConst>(), 0)) { error = "Could not encode Sony RPC parameters"; return false; }
  if (out.size() > MAX_WS_PAYLOAD) { error = "Sony RPC payload exceeds WebSocket limit"; return false; }
  return true;
}

bool sendWsFrame(int fd, uint8_t opcode, const uint8_t *payload, size_t length, String &error) {
  if (length > MAX_WS_PAYLOAD) { error = "WebSocket payload exceeds limit"; return false; }
  std::vector<uint8_t> frame;
  frame.reserve(length + 14);
  frame.push_back(static_cast<uint8_t>(0x80 | (opcode & 0x0F)));
  if (length <= 125) frame.push_back(static_cast<uint8_t>(0x80 | length));
  else { frame.push_back(0x80 | 126); appendU16(frame, static_cast<uint16_t>(length)); }
  uint32_t rnd = esp_random();
  uint8_t mask[4] = {
      static_cast<uint8_t>(rnd), static_cast<uint8_t>(rnd >> 8),
      static_cast<uint8_t>(rnd >> 16), static_cast<uint8_t>(rnd >> 24)};
  frame.insert(frame.end(), mask, mask + 4);
  for (size_t i = 0; i < length; ++i) frame.push_back(payload[i] ^ mask[i & 3]);
  return sendAll(fd, frame.data(), frame.size(), error);
}

bool recvWsMessage(int fd, std::vector<uint8_t> &payload, uint8_t &opcode, String &error) {
  payload.clear();
  opcode = 0;
  bool started = false;
  while (true) {
    uint8_t h[2];
    if (!recvExact(fd, h, 2, error)) return false;
    const bool fin = (h[0] & 0x80) != 0;
    const uint8_t op = h[0] & 0x0F;
    const bool masked = (h[1] & 0x80) != 0;
    uint64_t len = h[1] & 0x7F;
    if (masked) { error = "Camera sent an invalid masked WebSocket frame"; return false; }
    if (len == 126) {
      uint8_t x[2]; if (!recvExact(fd, x, 2, error)) return false;
      len = (static_cast<uint16_t>(x[0]) << 8) | x[1];
    } else if (len == 127) {
      uint8_t x[8]; if (!recvExact(fd, x, 8, error)) return false;
      len = 0; for (uint8_t b : x) len = (len << 8) | b;
    }
    if (len > MAX_WS_PAYLOAD || payload.size() + len > MAX_WS_PAYLOAD) {
      error = "Camera WebSocket frame exceeds limit";
      return false;
    }
    std::vector<uint8_t> part(static_cast<size_t>(len));
    if (len && !recvExact(fd, part.data(), part.size(), error)) return false;

    if (op == 0x8) { error = "Camera closed the WebSocket"; return false; }
    if (op == 0x9) { // ping
      if (!sendWsFrame(fd, 0xA, part.data(), part.size(), error)) return false;
      continue;
    }
    if (op == 0xA) continue;
    if (op == 0x1 || op == 0x2) {
      if (started) { error = "Unexpected new WebSocket message during fragmentation"; return false; }
      started = true; opcode = op;
    } else if (op == 0x0) {
      if (!started) { error = "Unexpected WebSocket continuation frame"; return false; }
    } else {
      error = "Unsupported WebSocket opcode";
      return false;
    }
    payload.insert(payload.end(), part.begin(), part.end());
    if (fin) return true;
  }
}

bool readLength(const std::vector<uint8_t> &buf, size_t &p, size_t bytes, uint64_t &value) {
  if (p + bytes > buf.size()) return false;
  value = 0;
  for (size_t i = 0; i < bytes; ++i) value = (value << 8) | buf[p++];
  return true;
}

bool readMsgpackUInt(const std::vector<uint8_t> &buf, size_t &p, uint64_t &value) {
  if (p >= buf.size()) return false;
  uint8_t c = buf[p++];
  if (c <= 0x7F) { value = c; return true; }
  uint64_t v = 0;
  switch (c) {
    case 0xCC: if (!readLength(buf, p, 1, v)) return false; break;
    case 0xCD: if (!readLength(buf, p, 2, v)) return false; break;
    case 0xCE: if (!readLength(buf, p, 4, v)) return false; break;
    case 0xCF: if (!readLength(buf, p, 8, v)) return false; break;
    default: return false;
  }
  value = v; return true;
}

bool readArrayLength(const std::vector<uint8_t> &buf, size_t &p, uint32_t &count) {
  if (p >= buf.size()) return false;
  uint8_t c = buf[p++];
  if ((c & 0xF0) == 0x90) { count = c & 0x0F; return true; }
  uint64_t v = 0;
  if (c == 0xDC && readLength(buf, p, 2, v)) { count = static_cast<uint32_t>(v); return true; }
  if (c == 0xDD && readLength(buf, p, 4, v) && v <= UINT32_MAX) { count = static_cast<uint32_t>(v); return true; }
  return false;
}

bool skipMsgpack(const std::vector<uint8_t> &buf, size_t &p, int depth, String *summary = nullptr) {
  if (depth > 16 || p >= buf.size()) return false;
  size_t start = p;
  uint8_t c = buf[p++];
  if (c <= 0x7F || c >= 0xE0 || c == 0xC0 || c == 0xC2 || c == 0xC3) {
    if (summary) {
      if (c == 0xC0) *summary = "null";
      else if (c == 0xC2) *summary = "false";
      else if (c == 0xC3) *summary = "true";
      else if (c <= 0x7F) *summary = String(c);
      else *summary = String(static_cast<int8_t>(c));
    }
    return true;
  }
  if ((c & 0xE0) == 0xA0) {
    size_t n = c & 0x1F; if (p + n > buf.size()) return false;
    if (summary) { String s; for (size_t i = 0; i < n && i < 96; ++i) s += static_cast<char>(buf[p+i]); *summary = s; }
    p += n; return true;
  }
  if ((c & 0xF0) == 0x90) {
    uint32_t n = c & 0x0F; for (uint32_t i=0;i<n;++i) if(!skipMsgpack(buf,p,depth+1)) return false; return true;
  }
  if ((c & 0xF0) == 0x80) {
    uint32_t n = c & 0x0F; for(uint32_t i=0;i<n;++i){if(!skipMsgpack(buf,p,depth+1)||!skipMsgpack(buf,p,depth+1))return false;} return true;
  }
  uint64_t n = 0;
  switch (c) {
    case 0xC4: if(!readLength(buf,p,1,n))return false; break;
    case 0xC5: if(!readLength(buf,p,2,n))return false; break;
    case 0xC6: if(!readLength(buf,p,4,n))return false; break;
    case 0xD9: if(!readLength(buf,p,1,n))return false; break;
    case 0xDA: if(!readLength(buf,p,2,n))return false; break;
    case 0xDB: if(!readLength(buf,p,4,n))return false; break;
    case 0xCA: n=4; break; case 0xCB: n=8; break;
    case 0xCC: n=1; break; case 0xCD: n=2; break; case 0xCE: n=4; break; case 0xCF: n=8; break;
    case 0xD0: n=1; break; case 0xD1: n=2; break; case 0xD2: n=4; break; case 0xD3: n=8; break;
    case 0xD4: n=2; break; case 0xD5: n=3; break; case 0xD6: n=5; break; case 0xD7: n=9; break; case 0xD8: n=17; break;
    case 0xC7: { uint64_t x; if(!readLength(buf,p,1,x))return false; n=x+1; break; }
    case 0xC8: { uint64_t x; if(!readLength(buf,p,2,x))return false; n=x+1; break; }
    case 0xC9: { uint64_t x; if(!readLength(buf,p,4,x))return false; n=x+1; break; }
    case 0xDC: { uint64_t x; if(!readLength(buf,p,2,x))return false; for(uint64_t i=0;i<x;++i)if(!skipMsgpack(buf,p,depth+1))return false; return true; }
    case 0xDD: { uint64_t x; if(!readLength(buf,p,4,x))return false; for(uint64_t i=0;i<x;++i)if(!skipMsgpack(buf,p,depth+1))return false; return true; }
    case 0xDE: { uint64_t x; if(!readLength(buf,p,2,x))return false; for(uint64_t i=0;i<x;++i){if(!skipMsgpack(buf,p,depth+1)||!skipMsgpack(buf,p,depth+1))return false;} return true; }
    case 0xDF: { uint64_t x; if(!readLength(buf,p,4,x))return false; for(uint64_t i=0;i<x;++i){if(!skipMsgpack(buf,p,depth+1)||!skipMsgpack(buf,p,depth+1))return false;} return true; }
    default: p = start; return false;
  }
  if (p + n > buf.size()) return false;
  if (summary && (c == 0xD9 || c == 0xDA || c == 0xDB)) {
    String s; for (size_t i=0;i<n && i<96;++i)s+=static_cast<char>(buf[p+i]); *summary=s;
  }
  p += static_cast<size_t>(n); return true;
}

bool parseRpcEnvelope(const std::vector<uint8_t> &buf, uint32_t wantedId,
                      bool &matched, bool &rpcOk, String &rpcError, String &resultSummary) {
  matched = false; rpcOk = false; rpcError = ""; resultSummary = "";
  size_t p = 0; uint32_t count = 0;
  if (!readArrayLength(buf, p, count) || count < 1) return false;
  uint64_t type = 0;
  if (!readMsgpackUInt(buf, p, type)) return false;

  // Sony /linear notifications use a different envelope (typically
  // [2, name, data]) from responses ([1, id, error, result]). They can arrive
  // between a request and its response, particularly after authentication.
  // Ignore complete non-response messages here rather than trying to parse
  // their second field as a numeric request id.
  if (type != 1) return true;
  if (count != 4) return false;

  uint64_t id = 0;
  if (!readMsgpackUInt(buf, p, id)) return false;
  if (id != wantedId) return true; // response to another request; ignore
  matched = true;
  if (p >= buf.size()) return false;
  if (buf[p] == 0xC0) { ++p; rpcOk = true; }
  else {
    String summary;
    if (!skipMsgpack(buf, p, 0, &summary)) return false;
    rpcError = summary.length() ? summary : "camera returned a non-null RPC error";
  }
  if (!skipMsgpack(buf, p, 0, &resultSummary)) return false;
  return true;
}

}

String SonyLinear::basicAuthorization() const {
  String plain = cfg_.cameraUsername + ":" + cfg_.cameraPassword;
  return base64Bytes(reinterpret_cast<const uint8_t *>(plain.c_str()), plain.length());
}

bool SonyLinear::resolveCamera(struct sockaddr_in &remote, String &error) const {
  memset(&remote, 0, sizeof(remote));
  remote.sin_family = AF_INET;
  remote.sin_port = htons(80);
  if (inet_pton(AF_INET, cfg_.cameraHost.c_str(), &remote.sin_addr) == 1) return true;
  struct addrinfo hints {};
  hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM;
  struct addrinfo *result = nullptr;
  int rc = getaddrinfo(cfg_.cameraHost.c_str(), "80", &hints, &result);
  if (rc != 0 || !result) { error = "Could not resolve camera host"; if (result) freeaddrinfo(result); return false; }
  remote = *reinterpret_cast<struct sockaddr_in *>(result->ai_addr);
  freeaddrinfo(result); return true;
}

int SonyLinear::openBoundSocket(struct sockaddr_in &remote, uint32_t timeoutMs, String &error) const {
  int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (fd < 0) { error = String("socket() failed: ") + strerror(errno); return -1; }
  struct timeval tv {};
  tv.tv_sec = timeoutMs / 1000; tv.tv_usec = (timeoutMs % 1000) * 1000;
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
  struct sockaddr_in local {};
  local.sin_family = AF_INET; local.sin_port = htons(0);
  local.sin_addr.s_addr = inet_addr(WiFi.localIP().toString().c_str());
  if (bind(fd, reinterpret_cast<struct sockaddr *>(&local), sizeof(local)) != 0) {
    error = String("bind(Wi-Fi source) failed: ") + strerror(errno); close(fd); return -1;
  }
  if (connect(fd, reinterpret_cast<struct sockaddr *>(&remote), sizeof(remote)) != 0) {
    error = String("connect() failed: ") + strerror(errno); close(fd); return -1;
  }
  return fd;
}

bool SonyLinear::websocketHandshake(int fd, uint32_t timeoutMs, String &error, int &httpStatus) const {
  (void)timeoutMs;
  uint8_t nonce[16]; esp_fill_random(nonce, sizeof(nonce));
  String key = base64Bytes(nonce, sizeof(nonce));
  if (!key.length()) { error = "Could not generate WebSocket key"; return false; }
  String req = "GET /linear HTTP/1.1\r\nHost: " + cfg_.cameraHost +
               "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: " + key +
               "\r\nSec-WebSocket-Version: 13\r\nOrigin: http://" + cfg_.cameraHost +
               "\r\nUser-Agent: FS7-Network-Bridge/" FS7B_VERSION + "\r\n";
  // /rm.html is protected by the FS7's HTTP Basic Authentication realm.
  // A browser reuses those credentials for the same-origin WebSocket upgrade;
  // our raw socket must do the equivalent explicitly.
  String auth = basicAuthorization();
  if (auth.length()) req += "Authorization: Basic " + auth + "\r\n";
  req += "Pragma: no-cache\r\nCache-Control: no-cache\r\n\r\n";
  if (!sendAll(fd, reinterpret_cast<const uint8_t *>(req.c_str()), req.length(), error)) return false;
  String headers, leftover;
  if (!readHttpHeaders(fd, headers, leftover, error)) return false;
  if (leftover.length()) { error = "Unexpected data immediately after WebSocket handshake"; return false; }
  httpStatus = parseHttpStatus(headers);
  if (httpStatus != 101) { error = "Sony /linear handshake returned HTTP " + String(httpStatus); return false; }
  String upgrade, connection, accept;
  if (!headerValue(headers, "upgrade", upgrade) || !headerValue(headers, "connection", connection) ||
      !headerValue(headers, "sec-websocket-accept", accept)) {
    error = "Sony /linear handshake is missing required WebSocket headers"; return false;
  }
  upgrade.toLowerCase(); connection.toLowerCase();
  if (upgrade != "websocket" || connection.indexOf("upgrade") < 0) {
    error = "Sony /linear handshake returned invalid Upgrade headers"; return false;
  }
  String material = key + WS_GUID;
  uint8_t sha[20];
  SHA1Builder sha1;
  sha1.begin();
  sha1.add(reinterpret_cast<const uint8_t *>(material.c_str()), material.length());
  sha1.calculate();
  sha1.getBytes(sha);
  String expected = base64Bytes(sha, sizeof(sha));
  if (accept != expected) { error = "Sony /linear Sec-WebSocket-Accept verification failed"; return false; }
  return true;
}

bool SonyLinear::sendRpc(int fd, uint32_t id, const String &method, const String &paramsJson,
                         uint32_t timeoutMs, String &error, String &resultSummary) {
  (void)timeoutMs;
  std::vector<uint8_t> rpc;
  if (!encodeRpc(id, method, paramsJson, rpc, error)) return false;
  if (!sendWsFrame(fd, 0x2, rpc.data(), rpc.size(), error)) return false;
  uint32_t started = millis();
  while (millis() - started <= timeoutMs) {
    std::vector<uint8_t> payload; uint8_t opcode = 0;
    if (!recvWsMessage(fd, payload, opcode, error)) return false;
    if (opcode != 0x2) continue;
    bool matched = false, rpcOk = false; String rpcError, summary;
    if (!parseRpcEnvelope(payload, id, matched, rpcOk, rpcError, summary)) {
      error = "Malformed MessagePack RPC response from camera"; return false;
    }
    if (!matched) continue;
    if (!rpcOk) {
      error = "Sony RPC error";
      if (rpcError.length()) { error += ": "; error += rpcError; }
      return false;
    }
    resultSummary = summary;
    return true;
  }
  error = "Timed out waiting for Sony RPC response";
  return false;
}

SonyResponse SonyLinear::request(const String &method, const String &paramsJson,
                                 uint32_t timeoutOverrideMs) {
  SonyResponse result;
  result.isLinear = true;
  if (WiFi.status() != WL_CONNECTED) { result.error = "ESP32 is not connected to the FS7 Wi-Fi network"; return result; }
  uint32_t timeoutMs = timeoutOverrideMs ? timeoutOverrideMs : cfg_.cameraTimeoutMs;
  timeoutMs = constrain(timeoutMs, 250U, 15000U);
  struct sockaddr_in remote {};
  if (!resolveCamera(remote, result.error)) return result;
  int fd = openBoundSocket(remote, timeoutMs, result.error);
  if (fd < 0) return result;
  int handshakeStatus = 0;
  if (!websocketHandshake(fd, timeoutMs, result.error, handshakeStatus)) { result.httpStatus = handshakeStatus; close(fd); return result; }
  result.httpStatus = 101;
  result.transportOk = true;
  // The FS7 rmt.html source creates a plain Savona client and calls client.connect()
  // directly. It does not issue a second application-layer authentication RPC
  // after the upgrade. HTTP Basic on /linear is the authentication boundary.
  String summary;
  uint32_t id = nextRequestId_++;
  if (!nextRequestId_) nextRequestId_ = 1;
  if (!sendRpc(fd, id, method, paramsJson, timeoutMs, result.error, summary)) { close(fd); return result; }
  result.commandOk = true;
  result.body = summary;
  result.rpcMethod = method;
  close(fd);
  return result;
}

SonyResponse SonyLinear::test() {
  // Proven directly by the FS7 native rmt.html / record_connection.js.
  // This is read-only and confirms the full WebSocket + MessagePack RPC path.
  return request("Property.GetValue", "[{\"P.Clip.Mediabox.Status\":null}]",
                 std::max<uint32_t>(cfg_.cameraTimeoutMs, 3000U));
}

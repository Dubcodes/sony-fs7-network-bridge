#include "SonyRemote.h"

#include <WiFi.h>
#include <lwip/sockets.h>
#include <lwip/netdb.h>
#include <mbedtls/base64.h>
#include <errno.h>
#include <algorithm>
#include <cstring>
#include <memory>

namespace {
bool validMethod(const String &method) {
  return method == "GET" || method == "POST" || method == "PUT" ||
         method == "PATCH" || method == "DELETE" || method == "HEAD";
}

bool forbiddenHeader(String name) {
  name.trim();
  name.toLowerCase();
  return name == "authorization" || name == "host" || name == "content-length" ||
         name == "connection" || name == "transfer-encoding";
}

enum class ChunkState { Incomplete, Complete, Invalid };

ChunkState decodeChunkedBody(const String &input, String &out, size_t maxBytes) {
  out = "";
  int pos = 0;
  while (pos < input.length()) {
    int lineEnd = input.indexOf("\r\n", pos);
    if (lineEnd < 0) return ChunkState::Incomplete;
    String sizeText = input.substring(pos, lineEnd);
    int semicolon = sizeText.indexOf(';');
    if (semicolon >= 0) sizeText = sizeText.substring(0, semicolon);
    sizeText.trim();
    char *end = nullptr;
    errno = 0;
    unsigned long chunkSize = strtoul(sizeText.c_str(), &end, 16);
    if (errno == ERANGE || !end || end == sizeText.c_str() || *end != '\0') return ChunkState::Invalid;
    pos = lineEnd + 2;
    if (chunkSize == 0) {
      if (input.length() < pos + 2) return ChunkState::Incomplete;
      if (input.substring(pos, pos + 2) == "\r\n") return ChunkState::Complete;
      int trailersEnd = input.indexOf("\r\n\r\n", pos);
      return trailersEnd < 0 ? ChunkState::Incomplete : ChunkState::Complete;
    }
    if (chunkSize > maxBytes || out.length() + chunkSize > maxBytes) return ChunkState::Invalid;
    if (static_cast<unsigned long>(input.length() - pos) < chunkSize + 2) return ChunkState::Incomplete;
    out += input.substring(pos, pos + chunkSize);
    pos += chunkSize;
    if (input.substring(pos, pos + 2) != "\r\n") return ChunkState::Invalid;
    pos += 2;
  }
  return ChunkState::Incomplete;
}

bool parseUnsignedDecimal(String value, size_t &out) {
  value.trim();
  if (!value.length()) return false;
  size_t parsed = 0;
  for (size_t i = 0; i < value.length(); ++i) {
    if (!isdigit(static_cast<unsigned char>(value[i]))) return false;
    const size_t digit = static_cast<size_t>(value[i] - '0');
    if (parsed > (SIZE_MAX - digit) / 10) return false;
    parsed = parsed * 10 + digit;
  }
  out = parsed;
  return true;
}
}

String SonyRemote::basicAuthorization() const {
  String plain = cfg_.cameraUsername + ":" + cfg_.cameraPassword;
  size_t required = 0;
  mbedtls_base64_encode(nullptr, 0, &required,
                        reinterpret_cast<const unsigned char *>(plain.c_str()),
                        plain.length());
  if (required == 0) return String();
  std::unique_ptr<unsigned char[]> out(new unsigned char[required + 1]);
  size_t written = 0;
  if (mbedtls_base64_encode(out.get(), required, &written,
                            reinterpret_cast<const unsigned char *>(plain.c_str()),
                            plain.length()) != 0) {
    return String();
  }
  out[written] = 0;
  return String(reinterpret_cast<char *>(out.get()));
}

bool SonyRemote::resolveCamera(struct sockaddr_in &remote, String &error) const {
  memset(&remote, 0, sizeof(remote));
  remote.sin_family = AF_INET;
  remote.sin_port = htons(80);

  if (inet_pton(AF_INET, cfg_.cameraHost.c_str(), &remote.sin_addr) == 1) {
    return true;
  }

  struct addrinfo hints {};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  struct addrinfo *result = nullptr;
  int rc = getaddrinfo(cfg_.cameraHost.c_str(), "80", &hints, &result);
  if (rc != 0 || result == nullptr) {
    error = "Could not resolve camera host";
    if (result) freeaddrinfo(result);
    return false;
  }
  remote = *reinterpret_cast<struct sockaddr_in *>(result->ai_addr);
  freeaddrinfo(result);
  return true;
}

SonyResponse SonyRemote::send(const SonyCommandMapping &mapping, size_t maxBodyBytes) {
  if (mapping.transport == "linear") {
    primeCameraSession();
    return linear_.request(mapping.rpcMethod, mapping.rpcParams);
  }
  return rawRequest(mapping.method, mapping.path, mapping.body,
                    mapping.contentType, mapping.extraHeaders, maxBodyBytes);
}

SonyResponse SonyRemote::testConnection() {
  SonyResponse result = rawRequest("GET", "/rm.html", "", "text/plain", "", 8192);
  if (result.transportOk && result.httpStatus >= 200 && result.httpStatus < 300) {
    cameraSessionPrimed_ = true;
    primedWifiIp_ = WiFi.localIP().toString();
  }
  return result;
}

SonyResponse SonyRemote::testLinear() {
  primeCameraSession();
  return linear_.test();
}

void SonyRemote::begin(CameraStateStore &cameraState, RuntimeStatus &status) {
  linear_.begin(cameraState, status);
}

void SonyRemote::loop() {
  if (WiFi.status() == WL_CONNECTED) primeCameraSession();
  linear_.loop();
}

SonyResponse SonyRemote::linearRequest(const String &method, const String &paramsJson,
                                       uint32_t timeoutMs) {
  primeCameraSession();
  return linear_.request(method, paramsJson, timeoutMs);
}

void SonyRemote::primeCameraSession() {
  if (WiFi.status() != WL_CONNECTED) {
    cameraSessionPrimed_ = false;
    primedWifiIp_ = "";
    return;
  }
  const String wifiIp = WiFi.localIP().toString();
  if (cameraSessionPrimed_ && primedWifiIp_ == wifiIp) return;
  // Some real FS7 sessions do not begin responding reliably until its native
  // remote entry page has been requested. Reproduce that harmless handshake
  // once per camera-Wi-Fi association before the first direct /linear command.
  SonyResponse warmup = rawRequest("GET", "/rm.html", "", "text/plain", "", 8192, 3000);
  const bool confirmed = warmup.transportOk && warmup.httpStatus >= 200 && warmup.httpStatus < 300;
  // Do not make every command pay another three-second warmup penalty if the
  // camera did not answer this optional request; /linear still gets its chance.
  cameraSessionPrimed_ = true;
  primedWifiIp_ = wifiIp;
  Serial.printf("[sony] /rm.html session warmup %s\n", confirmed ? "ok" : "not confirmed");
}

bool SonyRemote::wifiConnected() const { return WiFi.status() == WL_CONNECTED; }

SonyResponse SonyRemote::rawRequest(const String &methodIn, const String &pathIn,
                                    const String &body, const String &contentType,
                                    const String &extraHeaders,
                                    size_t maxBodyBytes,
                                    uint32_t timeoutOverrideMs) {
  SonyResponse result;
  uint32_t startMs = millis();

  if (WiFi.status() != WL_CONNECTED) {
    result.error = "ESP32 is not connected to the FS7 Wi-Fi network";
    return result;
  }

  String method = methodIn;
  method.toUpperCase();
  if (!validMethod(method)) {
    result.error = "Unsupported HTTP method";
    return result;
  }
  String path = pathIn;
  if (!path.startsWith("/")) path = "/" + path;
  if (path.length() > 2048 || path.indexOf('\r') >= 0 || path.indexOf('\n') >= 0 ||
      path.indexOf(' ') >= 0 || path.indexOf('\t') >= 0) {
    result.error = "Invalid path";
    return result;
  }
  if (body.length() > 32768 || extraHeaders.length() > 4096 || contentType.length() > 128 ||
      contentType.indexOf('\r') >= 0 || contentType.indexOf('\n') >= 0 ||
      cfg_.cameraHost.indexOf('\r') >= 0 || cfg_.cameraHost.indexOf('\n') >= 0) {
    result.error = "Request exceeds limits or contains invalid header text";
    return result;
  }
  maxBodyBytes = std::min<size_t>(maxBodyBytes, 32768);

  struct sockaddr_in remote {};
  if (!resolveCamera(remote, result.error)) return result;

  int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (fd < 0) {
    result.error = String("socket() failed: ") + strerror(errno);
    return result;
  }

  const uint32_t timeoutMs = timeoutOverrideMs ? timeoutOverrideMs : cfg_.cameraTimeoutMs;
  struct timeval tv {};
  tv.tv_sec = timeoutMs / 1000;
  tv.tv_usec = (timeoutMs % 1000) * 1000;
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

  // Bind the outgoing socket to the Wi-Fi STA address. This is deliberate:
  // the FS7 defaults to 192.168.1.1/16, which can overlap a wired production/LAN
  // subnet. Source binding tells lwIP that this connection belongs to Wi-Fi.
  struct sockaddr_in local {};
  local.sin_family = AF_INET;
  local.sin_port = htons(0);
  local.sin_addr.s_addr = inet_addr(WiFi.localIP().toString().c_str());
  if (bind(fd, reinterpret_cast<struct sockaddr *>(&local), sizeof(local)) != 0) {
    result.error = String("bind(Wi-Fi source) failed: ") + strerror(errno);
    close(fd);
    return result;
  }

  if (connect(fd, reinterpret_cast<struct sockaddr *>(&remote), sizeof(remote)) != 0) {
    result.error = String("connect() failed: ") + strerror(errno);
    close(fd);
    return result;
  }

  String request;
  request.reserve(512 + body.length() + extraHeaders.length());
  request += method + " " + path + " HTTP/1.1\r\n";
  request += "Host: " + cfg_.cameraHost + "\r\n";
  request += "User-Agent: FS7-Network-Bridge/" FS7B_VERSION "\r\n";
  String auth = basicAuthorization();
  if (auth.length()) request += "Authorization: Basic " + auth + "\r\n";
  request += "Accept: */*\r\n";
  if (body.length()) {
    request += "Content-Type: " + contentType + "\r\n";
    request += "Content-Length: " + String(body.length()) + "\r\n";
  }
  if (extraHeaders.length()) {
    int start = 0;
    while (start < extraHeaders.length()) {
      int end = extraHeaders.indexOf('\n', start);
      if (end < 0) end = extraHeaders.length();
      String line = extraHeaders.substring(start, end);
      line.trim();
      int colon = line.indexOf(':');
      if (line.length() && colon > 0 && line.indexOf('\r') < 0 &&
          !forbiddenHeader(line.substring(0, colon))) {
        request += line + "\r\n";
      }
      start = end + 1;
    }
  }
  request += "Connection: close\r\n\r\n";
  request += body;

  size_t totalSent = 0;
  while (totalSent < request.length()) {
    int n = ::send(fd, request.c_str() + totalSent, request.length() - totalSent, 0);
    if (n <= 0) {
      result.error = String("send() failed: ") + strerror(errno);
      close(fd);
      return result;
    }
    totalSent += static_cast<size_t>(n);
  }

  String raw;
  raw.reserve(std::min<size_t>(maxBodyBytes + 2048, 49152));
  char buffer[1024];
  int split = -1;
  bool peerClosed = false;
  while (split < 0) {
    int n = recv(fd, buffer, sizeof(buffer), 0);
    if (n == 0) { peerClosed = true; break; }
    if (n < 0) {
      result.error = (errno == EWOULDBLOCK || errno == EAGAIN)
          ? "Timed out before complete HTTP headers"
          : String("recv() failed: ") + strerror(errno);
      close(fd); return result;
    }
    raw.concat(buffer, static_cast<unsigned int>(n));
    split = raw.indexOf("\r\n\r\n");
    if ((split < 0 && raw.length() > 8192) || split > 8192) {
      result.error = "HTTP response headers exceed 8192 bytes";
      close(fd); return result;
    }
  }
  if (split < 0) {
    result.error = peerClosed ? "Connection closed before complete HTTP headers" : "Malformed HTTP response";
    close(fd); return result;
  }

  result.headers = raw.substring(0, split);
  String wireBody = raw.substring(split + 4);
  int statusLineEnd = result.headers.indexOf("\r\n");
  String statusLine = statusLineEnd >= 0 ? result.headers.substring(0, statusLineEnd) : result.headers;
  int firstSpace = statusLine.indexOf(' ');
  if (!statusLine.startsWith("HTTP/") || firstSpace < 0 || statusLine.length() < firstSpace + 4 ||
      !isdigit(static_cast<unsigned char>(statusLine[firstSpace + 1])) ||
      !isdigit(static_cast<unsigned char>(statusLine[firstSpace + 2])) ||
      !isdigit(static_cast<unsigned char>(statusLine[firstSpace + 3])) ||
      (statusLine.length() > firstSpace + 4 && statusLine[firstSpace + 4] != ' ')) {
    result.error = "Malformed HTTP status line";
    close(fd); return result;
  }
  result.httpStatus = statusLine.substring(firstSpace + 1, firstSpace + 4).toInt();
  if (result.httpStatus < 100 || result.httpStatus > 599) {
    result.error = "Invalid HTTP status code";
    close(fd); return result;
  }

  bool haveLength = false;
  size_t contentLength = 0;
  String transferEncoding;
  int lineStart = statusLineEnd >= 0 ? statusLineEnd + 2 : result.headers.length();
  while (lineStart < result.headers.length()) {
    int lineEnd = result.headers.indexOf("\r\n", lineStart);
    if (lineEnd < 0) lineEnd = result.headers.length();
    String line = result.headers.substring(lineStart, lineEnd);
    int colon = line.indexOf(':');
    if (colon <= 0) { result.error = "Malformed HTTP header line"; close(fd); return result; }
    String name = line.substring(0, colon); name.trim(); name.toLowerCase();
    String value = line.substring(colon + 1); value.trim();
    if (name == "content-length") {
      size_t parsed = 0;
      if (!parseUnsignedDecimal(value, parsed) || (haveLength && parsed != contentLength)) {
        result.error = "Invalid or conflicting Content-Length"; close(fd); return result;
      }
      haveLength = true; contentLength = parsed;
    } else if (name == "transfer-encoding") {
      if (transferEncoding.length()) transferEncoding += ",";
      transferEncoding += value;
    }
    lineStart = lineEnd + 2;
  }

  transferEncoding.toLowerCase();
  const bool chunked = transferEncoding.length() && transferEncoding.endsWith("chunked");
  if (transferEncoding.length() && !chunked) {
    result.error = "Unsupported Transfer-Encoding"; close(fd); return result;
  }
  if (chunked && haveLength) {
    result.error = "Ambiguous HTTP framing (both chunked and Content-Length)"; close(fd); return result;
  }
  const bool noBody = method == "HEAD" || (result.httpStatus >= 100 && result.httpStatus < 200) ||
                      result.httpStatus == 204 || result.httpStatus == 304;
  if (noBody) {
    result.body = ""; result.transportOk = true; close(fd);
  } else if (haveLength) {
    if (contentLength > maxBodyBytes) {
      result.error = "HTTP response body exceeds configured limit"; close(fd); return result;
    }
    while (wireBody.length() < contentLength) {
      int n = recv(fd, buffer, sizeof(buffer), 0);
      if (n == 0) { result.error = "Truncated HTTP response body"; close(fd); return result; }
      if (n < 0) {
        result.error = (errno == EWOULDBLOCK || errno == EAGAIN)
            ? "Timed out before complete HTTP response body"
            : String("recv() failed: ") + strerror(errno);
        close(fd); return result;
      }
      wireBody.concat(buffer, static_cast<unsigned int>(n));
    }
    result.body = wireBody.substring(0, contentLength);
    result.transportOk = true; close(fd);
  } else if (chunked) {
    ChunkState state = ChunkState::Incomplete;
    String decoded;
    while ((state = decodeChunkedBody(wireBody, decoded, maxBodyBytes)) == ChunkState::Incomplete) {
      if (wireBody.length() > maxBodyBytes + 8192) {
        result.error = "Chunked HTTP response exceeds framing limit"; close(fd); return result;
      }
      int n = recv(fd, buffer, sizeof(buffer), 0);
      if (n == 0) { result.error = "Truncated chunked HTTP response"; close(fd); return result; }
      if (n < 0) {
        result.error = (errno == EWOULDBLOCK || errno == EAGAIN)
            ? "Timed out before complete chunked HTTP response"
            : String("recv() failed: ") + strerror(errno);
        close(fd); return result;
      }
      wireBody.concat(buffer, static_cast<unsigned int>(n));
    }
    if (state == ChunkState::Invalid) {
      result.error = "Malformed or oversized chunked HTTP response"; close(fd); return result;
    }
    result.body = decoded; result.transportOk = true; close(fd);
  } else {
    while (!peerClosed) {
      if (wireBody.length() > maxBodyBytes) {
        result.error = "Close-delimited HTTP response exceeds configured limit"; close(fd); return result;
      }
      int n = recv(fd, buffer, sizeof(buffer), 0);
      if (n == 0) { peerClosed = true; break; }
      if (n < 0) {
        result.error = (errno == EWOULDBLOCK || errno == EAGAIN)
            ? "Timed out before close-delimited HTTP response completed"
            : String("recv() failed: ") + strerror(errno);
        close(fd); return result;
      }
      wireBody.concat(buffer, static_cast<unsigned int>(n));
    }
    if (wireBody.length() > maxBodyBytes) {
      result.error = "Close-delimited HTTP response exceeds configured limit"; close(fd); return result;
    }
    result.body = wireBody; result.transportOk = true; close(fd);
  }

  Serial.printf("[sony] %s %s -> HTTP %d in %lu ms\n", method.c_str(), path.c_str(),
                result.httpStatus, static_cast<unsigned long>(millis() - startMs));
  return result;
}

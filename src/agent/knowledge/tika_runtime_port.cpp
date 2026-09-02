#include "tika_runtime_port.hpp"

#include <cerrno>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <system_error>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace wuwe::agent::knowledge::detail {
namespace {

#ifdef _WIN32
class winsock_runtime {
public:
  winsock_runtime() {
    WSADATA data {};
    const int result = WSAStartup(MAKEWORD(2, 2), &data);
    if (result != 0) {
      throw std::system_error(result, std::system_category(), "WSAStartup failed");
    }
  }

  ~winsock_runtime() {
    WSACleanup();
  }
};

using native_socket = SOCKET;
using socket_length = int;
constexpr native_socket invalid_socket = INVALID_SOCKET;

void close_socket(native_socket socket) {
  closesocket(socket);
}

int last_socket_error() {
  return WSAGetLastError();
}
#else
using native_socket = int;
using socket_length = socklen_t;
constexpr native_socket invalid_socket = -1;

void close_socket(native_socket socket) {
  close(socket);
}

int last_socket_error() {
  return errno;
}
#endif

class scoped_socket {
public:
  explicit scoped_socket(native_socket value) : value_(value) {
  }

  ~scoped_socket() {
    if (value_ != invalid_socket) {
      close_socket(value_);
    }
  }

  scoped_socket(const scoped_socket&) = delete;
  scoped_socket& operator=(const scoped_socket&) = delete;

  native_socket get() const noexcept {
    return value_;
  }

private:
  native_socket value_ { invalid_socket };
};

bool is_loopback_address(const sockaddr* address) {
  if (address->sa_family == AF_INET) {
    const auto* ipv4 = reinterpret_cast<const sockaddr_in*>(address);
    return (ntohl(ipv4->sin_addr.s_addr) & 0xff000000U) == 0x7f000000U;
  }
  if (address->sa_family == AF_INET6) {
    const auto* ipv6 = reinterpret_cast<const sockaddr_in6*>(address);
    static constexpr unsigned char loopback[16] = {
      0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1,
    };
    return std::memcmp(&ipv6->sin6_addr, loopback, sizeof(loopback)) == 0;
  }
  return false;
}

} // namespace

int select_available_loopback_port(const std::string& host) {
#ifdef _WIN32
  static winsock_runtime winsock;
  (void)winsock;
#endif

  addrinfo hints {};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;

  addrinfo* addresses = nullptr;
  const int resolve_result = getaddrinfo(host.c_str(), "0", &hints, &addresses);
  if (resolve_result != 0) {
    throw std::runtime_error("failed to resolve Tika loopback host: " + host);
  }
  const std::unique_ptr<addrinfo, decltype(&freeaddrinfo)> resolved(addresses, freeaddrinfo);

  int last_error = 0;
  bool found_loopback = false;
  for (auto* address = addresses; address; address = address->ai_next) {
    if (!is_loopback_address(address->ai_addr)) {
      continue;
    }
    found_loopback = true;
    scoped_socket candidate(
      socket(address->ai_family, address->ai_socktype, address->ai_protocol));
    if (candidate.get() == invalid_socket) {
      last_error = last_socket_error();
      continue;
    }
#ifdef _WIN32
    const BOOL exclusive = TRUE;
    if (setsockopt(candidate.get(), SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
          reinterpret_cast<const char*>(&exclusive), sizeof(exclusive)) != 0) {
      last_error = last_socket_error();
      continue;
    }
#endif
    if (bind(candidate.get(), address->ai_addr,
          static_cast<socket_length>(address->ai_addrlen)) != 0) {
      last_error = last_socket_error();
      continue;
    }

    sockaddr_storage bound {};
    socket_length bound_size = sizeof(bound);
    if (getsockname(
          candidate.get(), reinterpret_cast<sockaddr*>(&bound), &bound_size) != 0) {
      last_error = last_socket_error();
      continue;
    }
    if (bound.ss_family == AF_INET) {
      return ntohs(reinterpret_cast<const sockaddr_in*>(&bound)->sin_port);
    }
    if (bound.ss_family == AF_INET6) {
      return ntohs(reinterpret_cast<const sockaddr_in6*>(&bound)->sin6_port);
    }
  }

  if (!found_loopback) {
    throw std::invalid_argument(
      "Tika automatic port selection requires a loopback host: " + host);
  }
  throw std::system_error(
    last_error, std::system_category(), "failed to select an available Tika port");
}

} // namespace wuwe::agent::knowledge::detail

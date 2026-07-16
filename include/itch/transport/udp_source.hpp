#pragma once

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstddef>
#include <cstring>
#include <stdexcept>
#include <string>

// UDP multicast receiver: the only piece that knows about sockets. It hands raw
// datagrams to the caller, who feeds them to MoldUdp64Framer.

namespace itch {

class UdpMulticastSource {
public:
    // Join `group`:`port` on local interface `iface` (0.0.0.0 = default).
    UdpMulticastSource(const std::string& group, std::uint16_t port,
                       const std::string& iface = "0.0.0.0") {
        fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (fd_ < 0) throw std::runtime_error("socket() failed");

        int yes = 1;
        ::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        addr.sin_port = htons(port);
        if (::bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            ::close(fd_);
            throw std::runtime_error("bind() failed");
        }

        ip_mreq mreq{};
        mreq.imr_multiaddr.s_addr = ::inet_addr(group.c_str());
        mreq.imr_interface.s_addr = ::inet_addr(iface.c_str());
        if (::setsockopt(fd_, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) != 0) {
            ::close(fd_);
            throw std::runtime_error("IP_ADD_MEMBERSHIP failed");
        }
    }

    ~UdpMulticastSource() {
        if (fd_ >= 0) ::close(fd_);
    }

    UdpMulticastSource(const UdpMulticastSource&) = delete;
    UdpMulticastSource& operator=(const UdpMulticastSource&) = delete;

    // Blocking receive of one datagram. Returns bytes received, negative on error.
    std::ptrdiff_t recv(unsigned char* buf, std::size_t cap) {
        return ::recvfrom(fd_, buf, cap, 0, nullptr, nullptr);
    }

    int fd() const noexcept { return fd_; }

private:
    int fd_{-1};
};

}  // namespace itch

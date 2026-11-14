// NorthRay Capture: the network under RTMP: a TCP connection (Winsock) and, for RTMPS, TLS over it
// through SChannel, Windows' own TLS (the server's certificate is checked against Windows' trusted
// roots and its name, as a browser would; nothing is skipped).
#pragma once

#include "ncap/rtmp.hpp"

#include <winsock2.h>
#include <ws2tcpip.h>
#define SECURITY_WIN32
#include <security.h>
#include <schannel.h>

#include <string>
#include <vector>

namespace ncap::net {

class Tcp : public rtmp::Transport {
public:
    ~Tcp() override { close(); }
    // False with a reason in plain words.
    bool open(const std::string& host, int port, int timeoutMs, std::string& error);
    bool send(const std::uint8_t* data, std::size_t size) override;
    int receive(std::uint8_t* data, std::size_t size, int timeoutMs) override;
    void close();
    // A send that would block longer than this fails (a stalled connection): the stream reconnects.
    void setSendTimeout(int ms);

private:
    SOCKET s_ = INVALID_SOCKET;
};

class Tls : public rtmp::Transport {
public:
    explicit Tls(Tcp& tcp) : tcp_(tcp) {}
    ~Tls() override;
    bool handshake(const std::string& host, int timeoutMs, std::string& error);
    bool send(const std::uint8_t* data, std::size_t size) override;
    int receive(std::uint8_t* data, std::size_t size, int timeoutMs) override;

private:
    Tcp& tcp_;
    CredHandle cred_{};
    CtxtHandle ctx_{};
    bool haveCred_ = false, haveCtx_ = false;
    SecPkgContext_StreamSizes sizes_{};
    std::vector<std::uint8_t> in_;      // encrypted bytes received, not yet decrypted
    std::vector<std::uint8_t> plain_;   // decrypted bytes not yet handed out
};

// Winsock, once for the process.
bool startNetworking();

}  // namespace ncap::net

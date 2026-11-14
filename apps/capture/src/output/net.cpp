#include "output/net.hpp"

#include <wincrypt.h>

#include <algorithm>
#include <cstring>

namespace ncap::net {

bool startNetworking() {
    static const bool ok = [] {
        WSADATA d;
        return WSAStartup(MAKEWORD(2, 2), &d) == 0;
    }();
    return ok;
}

// ---------------------------------------------------------------- TCP

bool Tcp::open(const std::string& host, int port, int timeoutMs, std::string& error) {
    close();
    if (!startNetworking()) {
        error = "Windows networking couldn't start.";
        return false;
    }
    addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) != 0 || !res) {
        error = "The server's name (" + host + ") couldn't be found. Check the address and the internet connection.";
        return false;
    }
    bool connected = false;
    for (addrinfo* a = res; a && !connected; a = a->ai_next) {
        s_ = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (s_ == INVALID_SOCKET) continue;
        u_long nb = 1;
        ioctlsocket(s_, FIONBIO, &nb);   // connect with a timeout
        ::connect(s_, a->ai_addr, (int)a->ai_addrlen);
        fd_set w, e;
        FD_ZERO(&w);
        FD_ZERO(&e);
        FD_SET(s_, &w);
        FD_SET(s_, &e);
        timeval tv{timeoutMs / 1000, (timeoutMs % 1000) * 1000};
        if (select(0, nullptr, &w, &e, &tv) > 0 && FD_ISSET(s_, &w)) {
            int err = 0, len = sizeof err;
            getsockopt(s_, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&err), &len);
            connected = err == 0;
        }
        if (!connected) {
            closesocket(s_);
            s_ = INVALID_SOCKET;
        }
    }
    freeaddrinfo(res);
    if (!connected) {
        error = "The server (" + host + ":" + std::to_string(port) + ") couldn't be reached.";
        return false;
    }
    u_long nb = 0;
    ioctlsocket(s_, FIONBIO, &nb);
    BOOL nodelay = TRUE;
    setsockopt(s_, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&nodelay), sizeof nodelay);
    int sndbuf = 1 << 20;
    setsockopt(s_, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&sndbuf), sizeof sndbuf);
    setSendTimeout(10000);
    return true;
}

void Tcp::setSendTimeout(int ms) {
    if (s_ == INVALID_SOCKET) return;
    DWORD t = (DWORD)ms;
    setsockopt(s_, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&t), sizeof t);
}

bool Tcp::send(const std::uint8_t* d, std::size_t n) {
    while (n) {
        const int w = ::send(s_, reinterpret_cast<const char*>(d), (int)std::min<std::size_t>(n, 1 << 20), 0);
        if (w <= 0) return false;
        d += w;
        n -= (std::size_t)w;
    }
    return true;
}

int Tcp::receive(std::uint8_t* d, std::size_t n, int timeoutMs) {
    if (s_ == INVALID_SOCKET) return -1;
    fd_set r;
    FD_ZERO(&r);
    FD_SET(s_, &r);
    timeval tv{timeoutMs / 1000, (timeoutMs % 1000) * 1000};
    const int ready = select(0, &r, nullptr, nullptr, &tv);
    if (ready == 0) return 0;
    if (ready < 0) return -1;
    const int got = recv(s_, reinterpret_cast<char*>(d), (int)n, 0);
    return got > 0 ? got : -1;
}

void Tcp::close() {
    if (s_ != INVALID_SOCKET) {
        shutdown(s_, SD_BOTH);
        closesocket(s_);
    }
    s_ = INVALID_SOCKET;
}

// ---------------------------------------------------------------- TLS (SChannel)

Tls::~Tls() {
    if (haveCtx_) DeleteSecurityContext(&ctx_);
    if (haveCred_) FreeCredentialsHandle(&cred_);
}

bool Tls::handshake(const std::string& host, int timeoutMs, std::string& error) {
    SCHANNEL_CRED sc{};
    sc.dwVersion = SCHANNEL_CRED_VERSION;
    // Windows' own choice of protocols (TLS 1.2 and 1.3 where it has them); the server's
    // certificate is checked automatically (no SCH_CRED_MANUAL_CRED_VALIDATION).
    sc.dwFlags = SCH_CRED_AUTO_CRED_VALIDATION | SCH_CRED_NO_DEFAULT_CREDS | SCH_USE_STRONG_CRYPTO;
    TimeStamp expiry;
    if (AcquireCredentialsHandleW(nullptr, const_cast<wchar_t*>(UNISP_NAME_W), SECPKG_CRED_OUTBOUND, nullptr, &sc, nullptr, nullptr, &cred_, &expiry) != SEC_E_OK) {
        error = "Windows' secure connection (TLS) couldn't start.";
        return false;
    }
    haveCred_ = true;
    std::wstring whost(host.begin(), host.end());
    const DWORD want = ISC_REQ_SEQUENCE_DETECT | ISC_REQ_REPLAY_DETECT | ISC_REQ_CONFIDENTIALITY | ISC_REQ_EXTENDED_ERROR | ISC_REQ_ALLOCATE_MEMORY |
                       ISC_REQ_STREAM;
    std::vector<std::uint8_t> buf;
    bool first = true;
    for (;;) {
        SecBuffer inBufs[2] = {{(unsigned long)buf.size(), SECBUFFER_TOKEN, buf.data()}, {0, SECBUFFER_EMPTY, nullptr}};
        SecBufferDesc inDesc{SECBUFFER_VERSION, 2, inBufs};
        SecBuffer outBuf{0, SECBUFFER_TOKEN, nullptr};
        SecBufferDesc outDesc{SECBUFFER_VERSION, 1, &outBuf};
        DWORD attrs = 0;
        const SECURITY_STATUS st = InitializeSecurityContextW(&cred_, first ? nullptr : &ctx_, whost.data(), want, 0, 0, first ? nullptr : &inDesc, 0,
                                                              &ctx_, &outDesc, &attrs, &expiry);
        if (first) haveCtx_ = true;
        first = false;
        if (outBuf.cbBuffer && outBuf.pvBuffer) {
            const bool sent = tcp_.send(static_cast<std::uint8_t*>(outBuf.pvBuffer), outBuf.cbBuffer);
            FreeContextBuffer(outBuf.pvBuffer);
            if (!sent) {
                error = "The secure connection failed while starting.";
                return false;
            }
        }
        if (st == SEC_E_OK) {
            // Bytes the server sent beyond the handshake are already application data.
            if (inBufs[1].BufferType == SECBUFFER_EXTRA && inBufs[1].cbBuffer)
                in_.assign(buf.end() - (long)inBufs[1].cbBuffer, buf.end());
            break;
        }
        if (st == SEC_I_CONTINUE_NEEDED || st == SEC_E_INCOMPLETE_MESSAGE) {
            if (st == SEC_I_CONTINUE_NEEDED) {
                if (inBufs[1].BufferType == SECBUFFER_EXTRA && inBufs[1].cbBuffer) buf.erase(buf.begin(), buf.end() - (long)inBufs[1].cbBuffer);
                else buf.clear();
                if (!buf.empty()) continue;
            }
            std::uint8_t tmp[16384];
            const int n = tcp_.receive(tmp, sizeof tmp, timeoutMs);
            if (n <= 0) {
                error = "The server stopped answering while setting up the secure connection.";
                return false;
            }
            buf.insert(buf.end(), tmp, tmp + n);
            continue;
        }
        if (st == SEC_E_UNTRUSTED_ROOT || st == SEC_E_CERT_EXPIRED || st == SEC_E_WRONG_PRINCIPAL || st == CERT_E_CN_NO_MATCH)
            error = "The server's security certificate isn't valid for " + host + ", so the connection was refused.";
        else
            error = "The secure connection couldn't be set up.";
        return false;
    }
    // Check the certificate ourselves too (as well as SChannel's own check): it must chain to a
    // root Windows trusts, be in date, and be for this server's name.
    {
        PCCERT_CONTEXT cert = nullptr;
        if (QueryContextAttributesW(&ctx_, SECPKG_ATTR_REMOTE_CERT_CONTEXT, &cert) != SEC_E_OK || !cert) {
            error = "The server didn't present a security certificate, so the connection was refused.";
            return false;
        }
        CERT_CHAIN_PARA para{};
        para.cbSize = sizeof para;
        LPSTR usages[] = {const_cast<LPSTR>(szOID_PKIX_KP_SERVER_AUTH)};
        para.RequestedUsage.dwType = USAGE_MATCH_TYPE_OR;
        para.RequestedUsage.Usage.cUsageIdentifier = 1;
        para.RequestedUsage.Usage.rgpszUsageIdentifier = usages;
        PCCERT_CHAIN_CONTEXT chain = nullptr;
        bool trusted = false;
        std::string why = "isn't valid for " + host;
        if (CertGetCertificateChain(nullptr, cert, nullptr, cert->hCertStore, &para, 0, nullptr, &chain) && chain) {
            HTTPSPolicyCallbackData https{};
            https.cbStruct = sizeof https;
            https.dwAuthType = AUTHTYPE_SERVER;
            https.pwszServerName = whost.data();
            CERT_CHAIN_POLICY_PARA policy{};
            policy.cbSize = sizeof policy;
            policy.pvExtraPolicyPara = &https;
            CERT_CHAIN_POLICY_STATUS status{};
            status.cbSize = sizeof status;
            // The chain itself must be whole and end at a trusted root (revocation isn't asked for).
            const DWORD chainErrors = chain->TrustStatus.dwErrorStatus & ~(DWORD)(CERT_TRUST_REVOCATION_STATUS_UNKNOWN | CERT_TRUST_IS_OFFLINE_REVOCATION);
            if (chainErrors & (CERT_TRUST_IS_UNTRUSTED_ROOT | CERT_TRUST_IS_PARTIAL_CHAIN)) why = "isn't signed by an authority Windows trusts";
            else if (chainErrors & CERT_TRUST_IS_NOT_TIME_VALID) why = "has expired (or isn't valid yet; check the PC's clock)";
            else if (chainErrors & CERT_TRUST_IS_REVOKED) why = "has been revoked";
            else if (chainErrors == CERT_TRUST_NO_ERROR) {
                trusted = CertVerifyCertificateChainPolicy(CERT_CHAIN_POLICY_SSL, chain, &policy, &status) && status.dwError == 0;
            } else why = "couldn't be checked";
            CertFreeCertificateChain(chain);
        }
        CertFreeCertificateContext(cert);
        if (!trusted) {
            error = "The server's security certificate " + why + ", so the connection was refused.";
            return false;
        }
    }
    if (QueryContextAttributesW(&ctx_, SECPKG_ATTR_STREAM_SIZES, &sizes_) != SEC_E_OK) {
        error = "The secure connection couldn't be set up.";
        return false;
    }
    return true;
}

bool Tls::send(const std::uint8_t* d, std::size_t n) {
    std::vector<std::uint8_t> msg;
    while (n) {
        const std::size_t chunk = std::min<std::size_t>(n, sizes_.cbMaximumMessage);
        msg.resize(sizes_.cbHeader + chunk + sizes_.cbTrailer);
        std::memcpy(msg.data() + sizes_.cbHeader, d, chunk);
        SecBuffer bufs[4] = {{sizes_.cbHeader, SECBUFFER_STREAM_HEADER, msg.data()},
                             {(unsigned long)chunk, SECBUFFER_DATA, msg.data() + sizes_.cbHeader},
                             {sizes_.cbTrailer, SECBUFFER_STREAM_TRAILER, msg.data() + sizes_.cbHeader + chunk},
                             {0, SECBUFFER_EMPTY, nullptr}};
        SecBufferDesc desc{SECBUFFER_VERSION, 4, bufs};
        if (EncryptMessage(&ctx_, 0, &desc, 0) != SEC_E_OK) return false;
        if (!tcp_.send(msg.data(), bufs[0].cbBuffer + bufs[1].cbBuffer + bufs[2].cbBuffer)) return false;
        d += chunk;
        n -= chunk;
    }
    return true;
}

int Tls::receive(std::uint8_t* d, std::size_t n, int timeoutMs) {
    for (;;) {
        if (!plain_.empty()) {
            const std::size_t k = std::min(n, plain_.size());
            std::memcpy(d, plain_.data(), k);
            plain_.erase(plain_.begin(), plain_.begin() + (long)k);
            return (int)k;
        }
        if (!in_.empty()) {   // decrypt whatever whole records have arrived
            SecBuffer bufs[4] = {{(unsigned long)in_.size(), SECBUFFER_DATA, in_.data()}, {0, SECBUFFER_EMPTY, nullptr}, {0, SECBUFFER_EMPTY, nullptr},
                                 {0, SECBUFFER_EMPTY, nullptr}};
            SecBufferDesc desc{SECBUFFER_VERSION, 4, bufs};
            const SECURITY_STATUS st = DecryptMessage(&ctx_, &desc, 0, nullptr);
            if (st == SEC_E_OK) {
                std::vector<std::uint8_t> extra;
                for (auto& b : bufs) {
                    if (b.BufferType == SECBUFFER_DATA && b.cbBuffer)
                        plain_.insert(plain_.end(), static_cast<std::uint8_t*>(b.pvBuffer), static_cast<std::uint8_t*>(b.pvBuffer) + b.cbBuffer);
                    if (b.BufferType == SECBUFFER_EXTRA && b.cbBuffer)
                        extra.assign(static_cast<std::uint8_t*>(b.pvBuffer), static_cast<std::uint8_t*>(b.pvBuffer) + b.cbBuffer);
                }
                in_.swap(extra);
                continue;
            }
            if (st != SEC_E_INCOMPLETE_MESSAGE) return -1;   // closed by the server, or broken
        }
        std::uint8_t tmp[16384];
        const int got = tcp_.receive(tmp, sizeof tmp, timeoutMs);
        if (got <= 0) return got;
        in_.insert(in_.end(), tmp, tmp + got);
    }
}

}  // namespace ncap::net

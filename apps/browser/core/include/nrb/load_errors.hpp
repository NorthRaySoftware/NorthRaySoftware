// Plain-words descriptions of page load failures.
//
// The values match WebView2's COREWEBVIEW2_WEB_ERROR_STATUS, so the engine
// layer converts with a cast; the core stays free of Windows headers.
#pragma once

#include <string>
#include <string_view>

namespace nrb {

enum class LoadFailure : int {
    Unknown = 0,
    CertificateNameMismatch = 1,
    CertificateExpired = 2,
    ClientCertificateError = 3,
    CertificateRevoked = 4,
    CertificateInvalid = 5,
    ServerUnreachable = 6,
    Timeout = 7,
    InvalidServerResponse = 8,
    ConnectionAborted = 9,
    ConnectionReset = 10,
    Disconnected = 11,
    CannotConnect = 12,
    HostNotFound = 13,
    Cancelled = 14,
    RedirectFailed = 15,
    Unexpected = 16,
    AuthenticationRequired = 17,
    ProxyAuthenticationRequired = 18,
};

struct LoadErrorText {
    std::string title;    // "This site couldn't be found"
    std::string detail;   // what probably happened and what to try
    bool canRetry = true;
    bool offerSearch = false;   // the address may have been meant as a search
    bool isCertificate = false; // no "continue anyway" in this version
};

// Failures that aren't worth an error page: the navigation was stopped or
// replaced by another one (also when a download takes over).
bool isSilentLoadFailure(LoadFailure f);

LoadErrorText describeLoadFailure(LoadFailure f, std::string_view host);

}  // namespace nrb

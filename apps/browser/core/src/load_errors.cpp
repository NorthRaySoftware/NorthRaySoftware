#include "nrb/load_errors.hpp"

namespace nrb {

bool isSilentLoadFailure(LoadFailure f) { return f == LoadFailure::Cancelled; }

LoadErrorText describeLoadFailure(LoadFailure f, std::string_view hostIn) {
    const std::string host = hostIn.empty() ? std::string("the site") : std::string(hostIn);
    LoadErrorText t;
    switch (f) {
        case LoadFailure::CertificateNameMismatch:
        case LoadFailure::CertificateExpired:
        case LoadFailure::CertificateRevoked:
        case LoadFailure::CertificateInvalid:
            t.isCertificate = true;
            t.title = "This site's identity couldn't be confirmed";
            if (f == LoadFailure::CertificateExpired) t.detail = "The certificate " + host + " sent has expired. ";
            else if (f == LoadFailure::CertificateNameMismatch) t.detail = "The certificate " + host + " sent belongs to a different site. ";
            else if (f == LoadFailure::CertificateRevoked) t.detail = "The certificate " + host + " sent has been withdrawn by whoever issued it. ";
            else t.detail = "The certificate " + host + " sent isn't valid, or wasn't issued by an authority Windows trusts. ";
            t.detail += "Someone could be pretending to be the site, so NorthRay Browser didn't open it. "
                        "If the site is yours, check its certificate; otherwise try again later.";
            break;
        case LoadFailure::ClientCertificateError:
            t.isCertificate = true;
            t.title = host + " didn't accept your certificate";
            t.detail = "The site asked for a personal certificate, and the one offered wasn't accepted. "
                       "Check the certificates installed in Windows.";
            break;
        case LoadFailure::HostNotFound:
            t.title = "This site couldn't be found";
            t.detail = "There's no website called " + host + ", or your network couldn't look it up. "
                       "Check the spelling, or search for it instead.";
            t.offerSearch = true;
            break;
        case LoadFailure::Disconnected:
            t.title = "You're not connected to the internet";
            t.detail = "Check your Wi-Fi or network cable, then try again.";
            break;
        case LoadFailure::CannotConnect:
        case LoadFailure::ServerUnreachable:
            t.title = host + " can't be reached";
            t.detail = "The site may be down, or not accept connections at this address. Check the address and your "
                       "network, or try again later.";
            break;
        case LoadFailure::Timeout:
            t.title = host + " took too long to respond";
            t.detail = "The site may be busy or down, or your connection may be slow. Try again in a moment.";
            break;
        case LoadFailure::ConnectionAborted:
        case LoadFailure::ConnectionReset:
            t.title = "The connection to " + host + " was interrupted";
            t.detail = "The site closed the connection before the page arrived. Try again.";
            break;
        case LoadFailure::InvalidServerResponse:
            t.title = host + " sent a response the browser couldn't read";
            t.detail = "The site may be having problems. Try again later.";
            break;
        case LoadFailure::RedirectFailed:
            t.title = host + " sent the browser somewhere it couldn't follow";
            t.detail = "The site redirected to an address that couldn't be opened, or redirected too many times.";
            break;
        case LoadFailure::AuthenticationRequired:
            t.title = host + " needs you to sign in";
            t.detail = "The site asked for a user name and password that weren't given.";
            break;
        case LoadFailure::ProxyAuthenticationRequired:
            t.title = "Your proxy server needs you to sign in";
            t.detail = "Windows is set to use a proxy server that asked for a user name and password. Check the proxy "
                       "settings in Windows.";
            break;
        case LoadFailure::Cancelled:
            t.title = "Loading was stopped";
            break;
        case LoadFailure::Unexpected:
        case LoadFailure::Unknown:
        default:
            t.title = "This page couldn't be loaded";
            t.detail = "Something went wrong while loading " + host + " (error " + std::to_string((int)f) + "). Try again.";
            break;
    }
    return t;
}

}  // namespace nrb

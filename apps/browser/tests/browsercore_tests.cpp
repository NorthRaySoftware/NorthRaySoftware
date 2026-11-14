// Unit tests for NorthRay Browser's portable core. Dependency-free like the
// other suites: run it, it prints each group and exits non-zero on failure.
#include "nrb/load_errors.hpp"
#include "nrb/omnibox.hpp"
#include "nrb/tab_model.hpp"
#include "nrb/url.hpp"

#include <cstdio>
#include <string>

static int g_failed = 0, g_passed = 0;

#define CHECK(cond)                                                                          \
    do {                                                                                     \
        if (cond) { ++g_passed; }                                                            \
        else { ++g_failed; std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); }   \
    } while (0)

#define CHECK_EQ(a, b)                                                                                     \
    do {                                                                                                   \
        const std::string va_ = (a), vb_ = (b);                                                            \
        if (va_ == vb_) { ++g_passed; }                                                                    \
        else { ++g_failed; std::printf("  FAIL %s:%d  %s\n    got      %s\n    expected %s\n", __FILE__, __LINE__, #a, va_.c_str(), vb_.c_str()); } \
    } while (0)

using namespace nrb;

static const SearchEngine& ddg() { return *findSearchEngine("duckduckgo"); }

static InputResult in(const char* s) { return interpretInput(s, ddg()); }

static bool isUrl(const char* s, const char* expected) {
    const InputResult r = in(s);
    if (r.kind != InputKind::Url || r.url != expected) {
        std::printf("    input \"%s\" -> kind %d url \"%s\" reason \"%s\"\n", s, (int)r.kind, r.url.c_str(), r.reason.c_str());
        return false;
    }
    return true;
}
static bool isSearch(const char* s) {
    const InputResult r = in(s);
    if (r.kind != InputKind::Search) {
        std::printf("    input \"%s\" -> kind %d url \"%s\"\n", s, (int)r.kind, r.url.c_str());
        return false;
    }
    return true;
}
static bool isRefused(const char* s) { return in(s).kind == InputKind::Refused && !in(s).reason.empty(); }

static void testOmniboxAddresses() {
    std::puts("omnibox: addresses");
    CHECK(isUrl("example.com", "https://example.com"));
    CHECK(isUrl("  example.com  ", "https://example.com"));
    CHECK(isUrl("www.bbc.co.uk/news", "https://www.bbc.co.uk/news"));
    CHECK(isUrl("example.com:8443/a?b=1#c", "https://example.com:8443/a?b=1#c"));
    CHECK(isUrl("https://example.com", "https://example.com"));
    CHECK(isUrl("HTTP://Example.com/Path", "HTTP://Example.com/Path"));
    CHECK(isUrl("http://example.com/a b", "http://example.com/a%20b"));
    CHECK(isUrl("xn--bcher-kva.example", "https://xn--bcher-kva.example"));
    CHECK(isUrl("bücher.de", "https://bücher.de"));
    CHECK(isUrl("about:blank", "about:blank"));
    CHECK(isUrl("chrome://sandbox", "chrome://sandbox"));
    CHECK(isUrl("chrome://version/", "chrome://version/"));
    CHECK(isUrl("edge://gpu", "edge://gpu"));
    CHECK(isUrl("view-source:https://example.com", "view-source:https://example.com"));
    CHECK(isUrl("8.8.8.8", "https://8.8.8.8"));
    // A dotted name that isn't a real site still opens (and gets an error
    // page with "Search instead"), so it's never leaked to a search engine.
    CHECK(isUrl("notes.txt", "https://notes.txt"));
}

static void testOmniboxPrivate() {
    std::puts("omnibox: local and private addresses are opened, never searched");
    CHECK(isUrl("localhost", "http://localhost"));
    CHECK(isUrl("localhost:3000", "http://localhost:3000"));
    CHECK(isUrl("localhost:3000/api", "http://localhost:3000/api"));
    CHECK(isUrl("app.localhost", "http://app.localhost"));
    CHECK(isUrl("192.168.1.1", "http://192.168.1.1"));
    CHECK(isUrl("10.0.0.5:8080", "http://10.0.0.5:8080"));
    CHECK(isUrl("172.20.1.1", "http://172.20.1.1"));
    CHECK(isUrl("127.0.0.1", "http://127.0.0.1"));
    CHECK(isUrl("[::1]:8000", "http://[::1]:8000"));
    CHECK(isUrl("nas.local", "http://nas.local"));
    CHECK(isUrl("printer.lan/status", "http://printer.lan/status"));
    CHECK(isUrl("router.home.arpa", "http://router.home.arpa"));
    CHECK(isUrl("wiki.corp", "http://wiki.corp"));
    CHECK(isUrl("intranet:8080", "http://intranet:8080"));
    CHECK(isUrl("printer/", "http://printer/"));
    CHECK(isLocalOrPrivateHost("LOCALHOST."));
    CHECK(!isLocalOrPrivateHost("172.32.0.1"));
    CHECK(!isLocalOrPrivateHost("example.com"));
    CHECK(!isLocalOrPrivateHost("8.8.8.8"));
}

static void testOmniboxFiles() {
    std::puts("omnibox: file paths");
    CHECK(isUrl("C:\\Users\\Me\\notes.txt", "file:///C:/Users/Me/notes.txt"));
    CHECK(isUrl("c:/temp/a b.html", "file:///c:/temp/a%20b.html"));
    CHECK(isUrl("D:\\Café\\x.pdf", "file:///D:/Caf%C3%A9/x.pdf"));
    CHECK(isUrl("file:///C:/x.html", "file:///C:/x.html"));
    CHECK(isRefused("\\\\server\\share\\x.txt"));
    CHECK(isRefused("file://server/share/x.txt"));
    CHECK(isRefused(".\\notes.txt"));
    CHECK(isRefused("../notes.txt"));
    // A path with spaces is still a path, not a search.
    CHECK(in("C:\\My Documents\\plan.txt").kind == InputKind::Url);
}

static void testOmniboxSearches() {
    std::puts("omnibox: searches");
    CHECK(isSearch("weather"));
    CHECK(isSearch("weather in glasgow"));
    CHECK(isSearch("how do I use C++20 modules"));
    CHECK(isSearch("note: buy milk"));
    CHECK(isSearch("someone@example.com"));
    CHECK(isSearch("1.5"));             // a number, not an address
    CHECK(isSearch("?example.com"));    // forced search
    CHECK_EQ(in("rust vs c++").url, "https://duckduckgo.com/?q=rust+vs+c%2B%2B");
    CHECK_EQ(in("?example.com").url, "https://duckduckgo.com/?q=example.com");
    CHECK_EQ(searchUrl("a&b=c", *findSearchEngine("google")), "https://www.google.com/search?q=a%26b%3Dc");
    CHECK_EQ(encodeQueryComponent("é"), "%C3%A9");
    CHECK(in("").kind == InputKind::Empty);
    CHECK(in("   ").kind == InputKind::Empty);
    CHECK(in("?").kind == InputKind::Empty);
    CHECK(findSearchEngine("nope") == nullptr);
    CHECK(defaultSearchEngine().id == "duckduckgo");
    for (const SearchEngine& e : builtInSearchEngines()) {
        CHECK(e.queryUrl.rfind("https://", 0) == 0);
        CHECK(e.queryUrl.find("{q}") != std::string::npos);
    }
}

static void testOmniboxRefusals() {
    std::puts("omnibox: refused");
    CHECK(isRefused("javascript:alert(1)"));
    CHECK(isRefused("JavaScript:alert(document.cookie)"));
    CHECK(isRefused("data:text/html,<h1>Bank</h1>"));
    CHECK(isRefused("chrome://settings"));
    CHECK(isRefused("chrome://flags"));
    CHECK(isRefused("edge://settings"));
    CHECK(isRefused("about:config"));
    CHECK(isRefused("mailto:someone@example.com"));
    CHECK(isRefused("view-source:file:///C:/x"));
    CHECK(isRefused("https://"));
    CHECK(isRefused(std::string(40000, 'a').c_str()));
}

static void testUrlDisplay() {
    std::puts("url: display and kinds");
    CHECK_EQ(displayUrl("https://example.com/"), "example.com");
    CHECK_EQ(displayUrl("https://example.com/a/b?c"), "example.com/a/b?c");
    CHECK_EQ(displayUrl("https://example.com:443/"), "example.com");
    CHECK_EQ(displayUrl("https://example.com:8443/"), "example.com:8443");
    CHECK_EQ(displayUrl("http://example.com/"), "http://example.com");
    CHECK_EQ(displayUrl("http://example.com/x"), "http://example.com/x");
    CHECK_EQ(displayUrl("https://user@evil.example/"), "https://user@evil.example/");  // never hide user info
    CHECK_EQ(displayUrl("about:blank"), "");
    CHECK_EQ(displayUrl("file:///C:/x.txt"), "file:///C:/x.txt");
    CHECK_EQ(hostOf("https://Sub.Example.com:8080/x"), "sub.example.com");
    CHECK_EQ(hostOf("about:blank"), "");

    SecurityInfo good{true, true, false, false};
    SecurityInfo badCert{true, true, true, false};
    SecurityInfo mixed{true, true, false, true};
    CHECK(classifySite("https://example.com/", good) == SiteKind::Secure);
    CHECK(classifySite("https://example.com/", {}) == SiteKind::Secure);  // not reported yet
    CHECK(classifySite("https://example.com/", badCert) == SiteKind::CertProblem);
    CHECK(classifySite("https://example.com/", mixed) == SiteKind::MixedContent);
    CHECK(classifySite("http://example.com/", good) == SiteKind::Insecure);
    CHECK(classifySite("file:///C:/x", {}) == SiteKind::LocalFile);
    CHECK(classifySite("chrome://version", {}) == SiteKind::Internal);
    CHECK(classifySite("about:blank", {}) == SiteKind::Blank);
    CHECK(classifySite("", {}) == SiteKind::Blank);
    CHECK(classifySite("data:text/html,x", {}) == SiteKind::Other);
    CHECK_EQ(siteLabel(SiteKind::Insecure), "Not secure");
    CHECK_EQ(siteLabel(SiteKind::Secure), "");
}

static void testLoadErrors() {
    std::puts("load errors");
    CHECK(isSilentLoadFailure(LoadFailure::Cancelled));
    CHECK(!isSilentLoadFailure(LoadFailure::HostNotFound));
    LoadErrorText t = describeLoadFailure(LoadFailure::HostNotFound, "exampel.com");
    CHECK(t.offerSearch);
    CHECK(t.detail.find("exampel.com") != std::string::npos);
    for (LoadFailure f : {LoadFailure::CertificateNameMismatch, LoadFailure::CertificateExpired, LoadFailure::CertificateRevoked,
                          LoadFailure::CertificateInvalid}) {
        t = describeLoadFailure(f, "example.com");
        CHECK(t.isCertificate);
        CHECK(!t.offerSearch);
        CHECK(t.detail.find("example.com") != std::string::npos);
    }
    t = describeLoadFailure(LoadFailure::Disconnected, "");
    CHECK(t.title.find("internet") != std::string::npos);
    t = describeLoadFailure(LoadFailure::Unknown, "x.com");
    CHECK(t.detail.find("x.com") != std::string::npos);
    t = describeLoadFailure((LoadFailure)99, "x.com");   // a newer engine's code
    CHECK(t.detail.find("99") != std::string::npos);
    // Every known failure has words.
    for (int i = 0; i <= 18; ++i) CHECK(!describeLoadFailure((LoadFailure)i, "x.com").title.empty());
}

static void testTabs() {
    std::puts("tabs");
    TabModel m;
    CHECK(m.empty());
    const int a = m.add("https://a.example", true, false);
    const int b = m.add("https://b.example", false, false);
    const int c = m.add("https://c.example", false, false);
    CHECK(m.activeId() == a);
    CHECK(m.indexOf(c) == 2);
    // Links opened from a go just after it, in order.
    const int l1 = m.add("https://l1.example", false, true, a);
    const int l2 = m.add("https://l2.example", false, true, a);
    CHECK(m.indexOf(l1) == 1);
    CHECK(m.indexOf(l2) == 2);
    CHECK(m.indexOf(b) == 3);
    // Closing the active tab activates the one to its right.
    CHECK(m.close(a));
    CHECK(m.activeId() == l1);
    CHECK(m.find(l2)->openerId == 0);   // opener gone
    // ...or the one to its left, at the end.
    m.activate(c);
    m.close(c);
    CHECK(m.activeId() == b);
    // Cycling and positions.
    m.activateIndex(0);
    CHECK(m.activeId() == l1);
    m.activateRelative(-1);
    CHECK(m.activeId() == b);
    m.activateRelative(1);
    CHECK(m.activeId() == l1);
    m.activateIndex(99);
    CHECK(m.activeId() == b);
    // Moving.
    CHECK(m.move(b, 0));
    CHECK(m.indexOf(b) == 0);
    CHECK(!m.move(999, 0));
    // Recently closed, most recent first; blank tabs aren't remembered.
    CHECK(m.hasClosed());
    ClosedTab r = m.popClosed();
    CHECK_EQ(r.url, "https://c.example");
    r = m.popClosed();
    CHECK_EQ(r.url, "https://a.example");
    CHECK(!m.hasClosed());
    const int blank = m.add("about:blank", false, false);
    m.close(blank);
    CHECK(!m.hasClosed());
    // The last tab closing leaves no active tab.
    while (!m.empty()) m.close(m.tabs().front().id);
    CHECK(m.activeId() == 0);
    CHECK(!m.close(12345));
    // The closed list is capped.
    for (int i = 0; i < 40; ++i) m.close(m.add("https://x" + std::to_string(i) + ".example", true, false));
    int n = 0;
    while (m.hasClosed()) { m.popClosed(); ++n; }
    CHECK(n == (int)TabModel::kMaxClosed);
}

int main() {
    testOmniboxAddresses();
    testOmniboxPrivate();
    testOmniboxFiles();
    testOmniboxSearches();
    testOmniboxRefusals();
    testUrlDisplay();
    testLoadErrors();
    testTabs();
    std::printf("\n%d passed, %d failed\n", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

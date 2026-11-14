// Unit tests for NorthRay Browser's portable core. Dependency-free like the
// other suites: run it, it prints each group and exits non-zero on failure.
#include "nrb/blocker.hpp"
#include "nrb/bookmarks.hpp"
#include "nrb/downloads.hpp"
#include "nrb/history.hpp"
#include "nrb/references.hpp"
#include "nrb/load_errors.hpp"
#include "nrb/omnibox.hpp"
#include "nrb/tab_model.hpp"
#include "nrb/url.hpp"
#include "nrb/weather.hpp"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
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
    CHECK(isSilentLoadFailure(LoadFailure::ConnectionAborted));   // downloads, pages that moved on
    CHECK(!isSilentLoadFailure(LoadFailure::ConnectionReset));
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

static void testBookmarks() {
    std::printf("bookmarks\n");
    std::string err;
    auto st = BookmarkStore::open(":memory:", err);
    CHECK(st != nullptr);
    if (!st) return;
    // The two fixed folders exist and stay.
    CHECK(st->isFolder(BookmarkStore::kBar) && st->isFolder(BookmarkStore::kOther));
    CHECK(!st->remove(BookmarkStore::kBar) && !st->rename(BookmarkStore::kOther, "x"));
    // Adding, in order, and at a position.
    const auto a = st->add(BookmarkStore::kBar, "Alpha", "https://alpha.example/");
    const auto b = st->add(BookmarkStore::kBar, "Beta", "https://beta.example/");
    const auto c = st->add(BookmarkStore::kBar, "Gamma", "https://gamma.example/", 0);
    CHECK(a && b && c);
    auto bar = st->children(BookmarkStore::kBar);
    CHECK(bar.size() == 3 && bar[0].id == c && bar[1].id == a && bar[2].id == b);
    CHECK(bar[0].position == 0 && bar[2].position == 2);
    // Only safe addresses.
    CHECK(!st->add(BookmarkStore::kBar, "x", "javascript:alert(1)"));
    CHECK(!st->add(BookmarkStore::kBar, "x", "data:text/html,hi"));
    CHECK(st->add(BookmarkStore::kOther, "", "file:///C:/notes.txt") != 0);
    CHECK(!st->add(999, "x", "https://x.example/"));
    // Finding.
    CHECK(st->findUrl("https://beta.example/") && st->findUrl("https://beta.example/")->id == b);
    CHECK(!st->findUrl("https://nowhere.example/"));
    CHECK(st->search("ALPHA").size() == 1 && st->search("example").size() == 3 && st->search("").size() == 4);
    CHECK(st->count() == 4);
    // Renaming, moving, folders.
    CHECK(st->rename(a, "  Alpha site  ") && st->get(a)->title == "Alpha site");
    const auto f = st->addFolder(BookmarkStore::kBar, "Reading");
    CHECK(f && st->isFolder(f));
    CHECK(st->move(b, f, 0));
    bar = st->children(BookmarkStore::kBar);
    CHECK(bar.size() == 3 && bar[0].id == c && bar[1].id == a && bar[2].id == f);
    CHECK(bar[0].position == 0 && bar[1].position == 1 && bar[2].position == 2);
    CHECK(st->children(f).size() == 1);
    CHECK(st->move(c, BookmarkStore::kBar, 2));   // within a folder, to the end
    bar = st->children(BookmarkStore::kBar);
    CHECK(bar[0].id == a && bar[1].id == f && bar[2].id == c);
    const auto sub = st->addFolder(f, "Inner");
    CHECK(!st->move(f, sub, 0));                  // not into itself
    // Icons are checked for size.
    CHECK(st->setIcon(a, 2, 2, std::vector<unsigned char>(16, 200)) && st->get(a)->icon.size() == 16);
    CHECK(!st->setIcon(a, 2, 2, std::vector<unsigned char>(15, 200)));
    // Deleting a folder takes its contents.
    CHECK(st->remove(f));
    CHECK(!st->get(b) && !st->get(sub) && st->children(BookmarkStore::kBar).size() == 2);
    CHECK(st->children(BookmarkStore::kBar)[1].position == 1);

    // Export, then import into a fresh store: the same bookmarks, in the same places.
    st->add(BookmarkStore::kBar, "Tom & Jerry <cartoon>", "https://example.com/?a=1&b=\"2\"");
    const std::string html = st->exportHtml();
    CHECK(html.find("NETSCAPE-Bookmark-file-1") != std::string::npos);
    CHECK(html.find("Tom &amp; Jerry &lt;cartoon&gt;") != std::string::npos);
    auto st2 = BookmarkStore::open(":memory:", err);
    CHECK(st2 && st2->importHtml(html, BookmarkStore::kOther) == 4);
    if (st2) {
        const auto bar2 = st2->children(BookmarkStore::kBar);
        CHECK(bar2.size() == 3 && bar2[0].title == "Alpha site" && bar2[2].title == "Tom & Jerry <cartoon>");
        CHECK(bar2.size() == 3 && bar2[2].url == "https://example.com/?a=1&b=\"2\"");
        CHECK(st2->children(BookmarkStore::kOther).size() == 1);   // straight into Other bookmarks, no extra folder
    }
    // Another browser's export: nested folders, entities, a bookmarklet that must be skipped.
    const std::string other =
        "<!DOCTYPE NETSCAPE-Bookmark-file-1>\n<DL><p>\n"
        "<DT><H3 ADD_DATE=\"1700000000\" PERSONAL_TOOLBAR_FOLDER=\"true\">Favorites bar</H3>\n<DL><p>\n"
        "  <DT><A HREF=\"https://news.example/\" ADD_DATE=\"1700000001\">News &#8211; daily</A>\n"
        "  <DT><H3>Work</H3>\n  <DL><p>\n    <DT><A href='https://work.example/'>Work</A>\n  </DL><p>\n"
        "  <DT><A HREF=\"javascript:void(0)\">Bookmarklet</A>\n"
        "</DL><p>\n<DT><A HREF=\"https://loose.example/\">Loose</A>\n</DL><p>\n";
    auto st3 = BookmarkStore::open(":memory:", err);
    CHECK(st3 && st3->importHtml(other, BookmarkStore::kOther) == 3);
    if (st3) {
        const auto bar3 = st3->children(BookmarkStore::kBar);
        CHECK(bar3.size() == 2 && bar3[0].title == "News \xE2\x80\x93 daily" && bar3[0].created == 1700000001);
        CHECK(bar3.size() == 2 && bar3[1].folder && st3->children(bar3[1].id).size() == 1);
        CHECK(st3->children(BookmarkStore::kOther).size() == 1 && st3->children(BookmarkStore::kOther)[0].title == "Loose");
    }

    // On disk: it keeps its contents, makes a daily backup, and recovers from damage.
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "nrb-bookmarks-test";
    fs::remove_all(dir);
    fs::create_directories(dir);
    const std::string path = (dir / "bookmarks.db").string();
    {
        auto d = BookmarkStore::open(path, err);
        CHECK(d && d->add(BookmarkStore::kBar, "Kept", "https://kept.example/"));
        CHECK(d && d->backupIfDue(1000000) && fs::exists(path + ".backup"));
        CHECK(d && !d->backupIfDue(1000000 + 3600));   // not again the same day
    }
    {
        auto d = BookmarkStore::open(path, err);
        CHECK(d && d->children(BookmarkStore::kBar).size() == 1 && d->children(BookmarkStore::kBar)[0].title == "Kept");
    }
    {
        std::ofstream(path, std::ios::binary | std::ios::trunc) << std::string(8192, 'x');   // ruin it
        fs::remove(path + "-wal");
        fs::remove(path + "-shm");
        std::string notice;
        auto d = BookmarkStore::open(path, err, &notice);
        CHECK(d != nullptr && notice.find("backup") != std::string::npos && fs::exists(path + ".damaged"));
        CHECK(d && d->children(BookmarkStore::kBar).size() == 1);   // restored from the backup
    }
    fs::remove_all(dir);
}

static void testHistory() {
    std::string err;
    auto h = HistoryStore::open(":memory:", err);
    CHECK(h != nullptr);
    if (!h) return;
    CHECK(!historyUrl("northray://bookmarks") && !historyUrl("about:blank") && !historyUrl("javascript:alert(1)") && !historyUrl("data:text/html,x"));
    CHECK(historyUrl("https://example.com/") && historyUrl("file:///C:/notes.txt"));
    const std::int64_t t = 1'800'000'000;
    const auto a = h->add("https://example.com/", "", t);
    CHECK(a != 0);
    h->setTitle(a, "Example");
    CHECK(h->add("https://example.com/", "", t + 60) == a);          // a reload within 30 minutes: the same visit, moved
    CHECK(h->count() == 1);
    CHECK(h->add("https://example.com/", "", t + 3600) != a);        // later: a new visit
    h->add("https://news.example.org/story", "A story about Owls", t + 4000);
    CHECK(h->add("about:blank", "", t + 4100) == 0);
    CHECK(h->count() == 3);
    auto all = h->list();
    CHECK(all.size() == 3 && all[0].title == "A story about Owls" && all[2].title == "Example");
    CHECK(h->list("owls").size() == 1);                               // any case, title
    CHECK(h->list("EXAMPLE.COM").size() == 2);                         // address
    CHECK(h->list("100%").empty());                                    // LIKE characters are literal
    CHECK(h->list({}, t + 4000).size() == 2);                          // before
    CHECK(h->removeUrl("https://example.com/") == 2 && h->count() == 1);
    h->add("https://a.example/", "A", t + 5000);
    CHECK(h->clearSince(t + 4500) == 1 && h->count() == 1);
    CHECK(h->prune(t + 4000 + HistoryStore::kKeepSeconds + 1) == 1 && h->count() == 0);
    h->add("https://b.example/", "B", t);
    CHECK(h->remove(h->list()[0].id) && h->count() == 0);
    CHECK(h->clearSince(0) == 0);
}

static void testDownloads() {
    std::string err;
    auto s = DownloadStore::open(":memory:", err);
    CHECK(s != nullptr);
    if (!s) return;
    Download d;
    d.url = "https://example.com/files/report.pdf?x=1";
    d.total = 12'000'000;
    const auto id = s->add(d);
    CHECK(id > 0 && s->all().size() == 1 && s->active() == 1);
    CHECK_EQ(s->find(id)->name(), "report.pdf");   // no file yet: from the address
    Download u = *s->find(id);
    u.path = "C:\\Users\\Ada\\Downloads\\report (1).pdf";
    u.received = 4'200'000;
    u.estimatedEnd = 1000 + 3;
    s->update(u);
    CHECK_EQ(s->find(id)->name(), "report (1).pdf");
    CHECK_EQ(downloadStatus(*s->find(id), 1000), "4.2 MB of 12 MB, 3 seconds left");
    CHECK(std::abs(s->progress() - 0.35) < 0.001);
    CHECK(!s->remove(id));                 // still going: not removed
    u.state = DownloadState::Done;
    u.received = u.total;
    s->update(u);
    CHECK_EQ(downloadStatus(*s->find(id), 1000), "Done, 12 MB");
    CHECK(s->active() == 0 && s->find(id)->finished > 0 && s->progress() < 0);
    Download e;
    e.url = "https://example.com/big.iso";
    const auto id2 = s->add(e);
    CHECK(s->all().front().id == id2);     // newest first
    CHECK_EQ(downloadStatus(*s->find(id2), 0), "0 bytes");
    CHECK(s->clearFinished() == 1 && s->all().size() == 1);
    CHECK_EQ(formatBytes(820'000), "820 KB");
    CHECK_EQ(formatBytes(1'500'000'000), "1.5 GB");
    // Running downloads from last time are stopped when the list opens again.
    const std::string path = (std::filesystem::temp_directory_path() / "nrb-downloads-test.db").string();
    std::filesystem::remove(path);
    {
        auto f = DownloadStore::open(path, err);
        Download r;
        r.url = "https://example.com/a.zip";
        f->add(r);
    }
    {
        auto f = DownloadStore::open(path, err);
        CHECK(f && f->all().size() == 1 && f->all()[0].state == DownloadState::Failed && f->active() == 0);
        if (f) CHECK_EQ(downloadStatus(f->all()[0], 0), "Failed: Stopped when the browser closed");
    }
    std::filesystem::remove(path);
}

static void testBlocker() {
    CHECK_EQ(siteOf("news.bbc.co.uk"), "bbc.co.uk");
    CHECK_EQ(siteOf("a.b.example.com"), "example.com");
    CHECK_EQ(siteOf("example.com"), "example.com");
    CHECK_EQ(siteOf("192.168.1.10"), "192.168.1.10");
    CHECK_EQ(siteOf("localhost"), "localhost");
    CHECK(isTrackerHost("stats.g.doubleclick.net") && isTrackerHost("doubleclick.net") && isTrackerHost("WWW.Google-Analytics.com"));
    CHECK(!isTrackerHost("notdoubleclick.net") && !isTrackerHost("google.com") && !isTrackerHost("www.youtube.com"));
    CHECK(isTrackerHost("adservice.google.com") && !isTrackerHost("mail.google.com"));
    // Third-party only: a service's own site isn't broken.
    CHECK(shouldBlock("https://www.googletagmanager.com/gtm.js?id=1", "https://www.bbc.co.uk/news"));
    CHECK(!shouldBlock("https://www.hotjar.com/static/app.js", "https://www.hotjar.com/pricing"));
    CHECK(!shouldBlock("https://cdn.example.com/app.js", "https://www.example.com/"));
    CHECK(shouldBlock("https://c.amazon-adsystem.com/aax2/apstag.js", "https://example.org/"));
    const auto f = blockerFilters();
    CHECK(f.size() == trackerDomains().size() * 2 && f[0] == "*://doubleclick.net/*" && f[1] == "*://*.doubleclick.net/*");
}

static void testReferences() {
    std::string err;
    auto s = ReferenceStore::open(":memory:", err);
    CHECK(s != nullptr);
    if (!s) return;
    CHECK(s->boards().size() == 1 && s->boards()[0].name == "References");
    const auto chars = s->addBoard("  Characters ");
    CHECK(chars && s->addBoard("characters") == 0 && s->addBoard("   ") == 0);   // names are unique, any case
    RefPicture p;
    p.board = chars;
    p.file = "a1.png";
    p.source = "https://example.com/dragon.png";
    p.page = "https://example.com/dragons";
    p.title = "Dragons";
    p.width = 800;
    p.height = 600;
    const auto pid = s->addPicture(p);
    CHECK(pid && s->pictures(chars).size() == 1 && s->hasSource(chars, p.source) && !s->hasSource(s->firstBoard(), p.source));
    p.board = s->firstBoard();
    const auto pid2 = s->addPicture(p);   // the same file on two boards
    CHECK(s->removePicture(pid2).empty());  // still used: the file stays
    CHECK(s->boards()[1].pictures == 1);
    CHECK(s->addColour({0, chars, 0x1F3B2C, "https://example.com/", 0}) == s->addColour({0, chars, 0x1F3B2C, "", 0}));
    CHECK(s->colours(chars).size() == 1 && hexColour(s->colours(chars)[0].rgb) == "#1F3B2C");
    CHECK(s->renameBoard(chars, "People") && s->boards()[1].name == "People");
    const auto gone = s->removeBoard(chars);
    CHECK(gone.size() == 1 && gone[0] == "a1.png" && s->boards().size() == 1 && s->colours(chars).empty());
    s->removeBoard(s->firstBoard());
    CHECK(s->boards().size() == 1);          // there's always a board
    // Main colours: three-quarters red, a quarter blue; transparent ignored.
    std::vector<std::uint8_t> px(40 * 40 * 4);
    for (int i = 0; i < 40 * 40; ++i) {
        const bool red = i % 4 != 0;
        px[i * 4] = red ? 200 : 20; px[i * 4 + 1] = red ? 30 : 40; px[i * 4 + 2] = red ? 30 : 210; px[i * 4 + 3] = i < 40 ? 0 : 255;
    }
    const auto mc = mainColours(px.data(), 40, 40, 4);
    CHECK(mc.size() == 2 && mc[0] == 0xC81E1E && mc[1] == 0x1428D2);
}

static void testWeather() {
    const auto places = parsePlaces(R"({"results":[{"name":"Glasgow","latitude":55.86515,"longitude":-4.25763,"country":"United Kingdom","admin1":"Scotland"},
        {"name":"Glasgow","latitude":37.0,"longitude":-85.9,"country":"United States","admin1":"Kentucky"},{"name":"","latitude":1,"longitude":2}]})");
    CHECK(places.size() == 2 && places[0].label() == "Glasgow, Scotland, United Kingdom" && std::abs(places[1].latitude - 37.0) < 1e-9);
    CHECK(parsePlaces("{}").empty() && parsePlaces("not json").empty());
    CHECK_EQ(placeSearchUrl("Port Ellen"), "https://geocoding-api.open-meteo.com/v1/search?name=Port%20Ellen&count=6&language=en&format=json");
    CHECK(forecastUrl(places[0]).find("latitude=55.865") != std::string::npos && forecastUrl(places[0]).find("&longitude=-4.2576&current=temperature_2m,weather_code") != std::string::npos);
    const Weather w = parseForecast(R"({"current":{"time":"2026-11-14T15:45","interval":900,"temperature_2m":7.6,"weather_code":61,"wind_speed_10m":18.4,"is_day":1},
        "daily":{"time":["2026-11-14"],"temperature_2m_max":[9.1],"temperature_2m_min":[3.2]}})");
    CHECK(w.ok && w.code == 61 && w.words() == "Light rain" && std::abs(w.high - 9.1) < 1e-9 && std::abs(w.low - 3.2) < 1e-9 && w.day);
    CHECK(!parseForecast(R"({"current":{}})").ok);
    CHECK_EQ(temperatureText(7.6, false), "8\xC2\xB0" "C");
    CHECK_EQ(temperatureText(10, true), "50\xC2\xB0" "F");
}

int main() {
    testWeather();
    testReferences();
    testDownloads();
    testBlocker();
    testHistory();
    testBookmarks();
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

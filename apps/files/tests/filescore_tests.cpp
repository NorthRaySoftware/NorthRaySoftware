// Unit tests for NorthRay Files' portable core. Dependency-free like the other
// suites: run it, it prints each group and exits non-zero on failure.
#include "nrf/listing.hpp"
#include "nrf/paths.hpp"

#include <cstdio>
#include <string>

using namespace nrf;

static int g_failed = 0, g_passed = 0;

static std::string u8(const std::wstring& w) {
    std::string o;
    for (wchar_t c : w) o += c < 128 ? (char)c : '?';
    return o;
}

#define CHECK(cond)                                                                        \
    do {                                                                                   \
        if (cond) ++g_passed;                                                              \
        else { ++g_failed; std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } \
    } while (0)

#define CHECK_EQ(a, b)                                                                                              \
    do {                                                                                                            \
        const std::wstring va_ = (a), vb_ = (b);                                                                    \
        if (va_ == vb_) ++g_passed;                                                                                 \
        else {                                                                                                      \
            ++g_failed;                                                                                             \
            std::printf("  FAIL %s:%d  %s\n    got      %s\n    expected %s\n", __FILE__, __LINE__, #a, u8(va_).c_str(), \
                        u8(vb_).c_str());                                                                           \
        }                                                                                                           \
    } while (0)

static std::wstring norm(const wchar_t* typed, const wchar_t* current = L"C:\\Users\\CJ") {
    auto r = normalizePath(typed, current);
    return r ? *r : L"<refused>";
}

static void testNormalize() {
    std::printf("paths: typed and pasted\n");
    CHECK_EQ(norm(L"C:\\Users"), L"C:\\Users");
    CHECK_EQ(norm(L"c:/users/cj/"), L"C:\\users\\cj");
    CHECK_EQ(norm(L"C:"), L"C:\\");
    CHECK_EQ(norm(L"C:\\"), L"C:\\");
    CHECK_EQ(norm(L"  \"D:\\Art Work\\\"  "), L"D:\\Art Work");
    CHECK_EQ(norm(L"C:\\Users\\\\CJ\\.\\Documents"), L"C:\\Users\\CJ\\Documents");
    CHECK_EQ(norm(L"C:\\Users\\CJ\\..\\..\\..\\.."), L"C:\\");               // never above the root
    CHECK_EQ(norm(L"\\\\server\\share"), L"\\\\server\\share");
    CHECK_EQ(norm(L"\\\\server\\share\\folder\\..\\x\\"), L"\\\\server\\share\\x");
    CHECK_EQ(norm(L"//server/share/a"), L"\\\\server\\share\\a");
    CHECK_EQ(norm(L"\\\\?\\C:\\Very\\Long"), L"C:\\Very\\Long");
    CHECK_EQ(norm(L"\\\\?\\UNC\\server\\share\\x"), L"\\\\server\\share\\x");
    CHECK_EQ(norm(L"file:///C:/My%20Docs/caf%C3%A9"), L"C:\\My Docs\\caf\u00e9");
    CHECK_EQ(norm(L"file://server/share/x"), L"\\\\server\\share\\x");
    // Relative to the current folder.
    CHECK_EQ(norm(L".."), L"C:\\Users");
    CHECK_EQ(norm(L"Documents\\Books"), L"C:\\Users\\CJ\\Documents\\Books");
    CHECK_EQ(norm(L"\\Windows"), L"C:\\Windows");
    CHECK_EQ(norm(L"..", L"C:\\"), L"C:\\");
    // Unicode names pass through untouched.
    CHECK_EQ(norm(L"C:\\\u65e5\u672c\u8a9e\\\U0001F319 moon"), L"C:\\\u65e5\u672c\u8a9e\\\U0001F319 moon");
    // Refused, with a reason.
    std::wstring why;
    CHECK(!normalizePath(L"", L"C:\\", &why) && !why.empty());
    CHECK(!normalizePath(L"C:folder", L"C:\\", &why) && !why.empty());
    CHECK(!normalizePath(L"C:\\bad|name", L"", &why));
    CHECK(!normalizePath(L"C:\\what?", L"", &why));
    CHECK(!normalizePath(L"C:\\trailing.", L"", &why));
    CHECK(!normalizePath(L"\\\\server", L"", &why));
    CHECK(!normalizePath(L"\\\\.\\PhysicalDrive0", L"", &why));
    CHECK(!normalizePath(L"Documents", L"", &why));
    CHECK(!normalizePath(L"\\Windows", L"\\\\server\\share", &why));
}

static void testPathParts() {
    std::printf("paths: parts\n");
    CHECK(isDriveRoot(L"C:\\") && !isDriveRoot(L"C:\\x") && isRoot(L"\\\\s\\share") && !isRoot(L"\\\\s\\share\\x"));
    CHECK_EQ(parentOf(L"C:\\Users\\CJ"), L"C:\\Users");
    CHECK_EQ(parentOf(L"C:\\Users"), L"C:\\");
    CHECK_EQ(parentOf(L"C:\\"), L"");
    CHECK_EQ(parentOf(L"\\\\s\\share\\x"), L"\\\\s\\share");
    CHECK_EQ(parentOf(L"\\\\s\\share"), L"");
    CHECK_EQ(nameOf(L"C:\\Users\\CJ"), L"CJ");
    CHECK_EQ(nameOf(L"C:\\"), L"C:");
    CHECK_EQ(join(L"C:\\", L"x"), L"C:\\x");
    CHECK_EQ(join(L"C:\\a", L"x"), L"C:\\a\\x");
    CHECK_EQ(extendedPath(L"C:\\a"), L"\\\\?\\C:\\a");
    CHECK_EQ(extendedPath(L"\\\\s\\share\\a"), L"\\\\?\\UNC\\s\\share\\a");
    auto c = breadcrumbs(L"C:\\Users\\CJ");
    CHECK(c.size() == 3 && c[0].label == L"C:" && c[0].path == L"C:\\" && c[1].path == L"C:\\Users" && c[2].label == L"CJ");
    c = breadcrumbs(L"C:\\");
    CHECK(c.size() == 1 && c[0].path == L"C:\\");
    c = breadcrumbs(L"\\\\s\\share\\a");
    CHECK(c.size() == 2 && c[0].label == L"\\\\s\\share" && c[1].path == L"\\\\s\\share\\a");
    CHECK(validName(L"Chapter 1.docx") && !validName(L"a:b") && !validName(L"..") && !validName(L"x ") && !validName(L""));
    CHECK_EQ(extensionOf(L"Cover.PNG"), L".png");
    CHECK_EQ(extensionOf(L"archive.tar.gz"), L".gz");
    CHECK_EQ(extensionOf(L"README"), L"");
    CHECK_EQ(extensionOf(L".gitignore"), L".gitignore");   // as Windows sees it
    CHECK_EQ(extensionOf(L"name."), L"");
}

static Entry file(const wchar_t* n, std::uint64_t size = 0, std::uint64_t when = 0, std::uint32_t a = 0, const wchar_t* type = L"") {
    Entry e;
    e.name = n;
    e.size = size;
    e.modified = when;
    e.attributes = a;
    e.type = type;
    return e;
}

static void testSorting() {
    std::printf("listing: sorting and hidden items\n");
    CHECK(naturalCompare(L"file2", L"file10") < 0);
    CHECK(naturalCompare(L"File10", L"file2") > 0);
    CHECK(naturalCompare(L"chapter 9", L"Chapter 09") < 0);    // same number: fewer zeros first
    CHECK(naturalCompare(L"a", L"A") != 0);                     // a steady order even when only case differs
    CHECK(naturalCompare(L"abc", L"ABD") < 0);
    CHECK(naturalCompare(L"x", L"x1") < 0);
    CHECK(naturalCompare(L"99999999999999999999", L"100000000000000000000") < 0);   // longer than any integer

    std::vector<Entry> v = {file(L"b10.txt", 5, 3, 0, L"Text"), file(L"Folder2", 0, 1, attr::Directory),
                            file(L"b2.txt", 50, 1, 0, L"Text"), file(L"a.png", 500, 2, 0, L"PNG image"),
                            file(L"folder10", 0, 9, attr::Directory)};
    sortEntries(v, {SortKey::Name, false});
    CHECK_EQ(v[0].name, L"Folder2");
    CHECK_EQ(v[1].name, L"folder10");
    CHECK_EQ(v[2].name, L"a.png");
    CHECK_EQ(v[3].name, L"b2.txt");
    CHECK_EQ(v[4].name, L"b10.txt");
    sortEntries(v, {SortKey::Name, true});
    CHECK(v[0].folder() && v[1].folder() && v[2].name == L"b10.txt");   // folders still first
    sortEntries(v, {SortKey::Size, true});
    CHECK_EQ(v[2].name, L"a.png");
    sortEntries(v, {SortKey::Modified, false});
    CHECK_EQ(v[0].name, L"Folder2");
    CHECK_EQ(v[2].name, L"b2.txt");
    sortEntries(v, {SortKey::Type, false});
    CHECK_EQ(v[2].name, L"a.png");

    const Entry plain = file(L"x"), hidden = file(L"h", 0, 0, attr::Hidden), sys = file(L"s", 0, 0, attr::Hidden | attr::System);
    CHECK(isVisible(plain, {}) && !isVisible(hidden, {}) && !isVisible(sys, {}));
    CHECK(isVisible(hidden, {true, false}) && !isVisible(sys, {true, false}));
    CHECK(isVisible(sys, {true, true}));
    CHECK(file(L"c", 0, 0, attr::RecallOnDataAccess).cloudOnly());
}

static void testSizes() {
    std::printf("listing: sizes\n");
    CHECK_EQ(formatSize(0), L"0 bytes");
    CHECK_EQ(formatSize(1), L"1 byte");
    CHECK_EQ(formatSize(1023), L"1023 bytes");
    CHECK_EQ(formatSize(1024), L"1.00 KB");
    CHECK_EQ(formatSize(2047), L"1.99 KB");
    CHECK_EQ(formatSize(15 * 1024 + 512), L"15.5 KB");
    CHECK_EQ(formatSize(1000 * 1024), L"0.97 MB");
    CHECK_EQ(formatSize(5ull * 1024 * 1024 * 1024), L"5.00 GB");
    CHECK_EQ(formatSizeKb(0), L"0 KB");
    CHECK_EQ(formatSizeKb(1), L"1 KB");
    CHECK_EQ(formatSizeKb(1024 * 1024 + 1), L"1,025 KB");
}

static void testSelection() {
    std::printf("listing: selection\n");
    Selection s;
    s.reset(10);
    CHECK(s.count() == 0 && s.focus() == -1);
    s.click(2, false, false);
    CHECK(s.selected(2) && s.count() == 1 && s.focus() == 2);
    s.click(5, false, true);                       // Shift: 2..5
    CHECK(s.count() == 4 && s.selected(3) && s.selected(5));
    s.click(8, true, false);                       // Ctrl: add 8
    CHECK(s.count() == 5 && s.selected(8));
    s.click(3, true, false);                       // Ctrl: remove 3
    CHECK(s.count() == 4 && !s.selected(3));
    s.click(1, false, false);
    CHECK(s.count() == 1 && s.selected(1));
    s.moveTo(4, false, true);                      // Shift+arrow from the anchor
    CHECK(s.count() == 4 && s.selected(1) && s.selected(4) && s.focus() == 4);
    s.moveTo(2, false, true);                      // back towards the anchor shrinks it
    CHECK(s.count() == 2);
    s.moveTo(7, true, false);                      // Ctrl+arrow: focus only
    CHECK(s.count() == 2 && s.focus() == 7);
    s.toggleFocused();
    CHECK(s.count() == 3 && s.selected(7));
    s.moveTo(99, false, false);                    // clamped to the last row
    CHECK(s.focus() == 9 && s.count() == 1);
    s.selectAll();
    CHECK(s.count() == 10 && s.indices().size() == 10);
    s.clickEmpty(true);
    CHECK(s.count() == 10);
    s.clickEmpty(false);
    CHECK(s.count() == 0);
    s.reset(0);
    s.moveTo(0, false, false);                     // an empty folder: nothing happens
    CHECK(s.focus() == -1);
}

int main() {
    testNormalize();
    testPathParts();
    testSorting();
    testSizes();
    testSelection();
    std::printf("\n%d passed, %d failed\n", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

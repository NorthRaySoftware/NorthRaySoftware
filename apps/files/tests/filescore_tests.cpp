// Unit tests for NorthRay Files' portable core. Dependency-free like the other
// suites: run it, it prints each group and exits non-zero on failure.
#include "nrf/art.hpp"
#include "nrf/exif.hpp"
#include "nrf/listing.hpp"
#include "nrf/operations.hpp"
#include "nrf/paths.hpp"
#include "nrf/search.hpp"

#include <cstdio>
#include <vector>
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

// A JPEG start with an EXIF block holding one tag (orientation), in either byte order.
static std::vector<unsigned char> jpegWithOrientation(int o, bool little, bool withApp0 = true) {
    std::vector<unsigned char> d = {0xFF, 0xD8};
    if (withApp0) { unsigned char a0[] = {0xFF, 0xE0, 0x00, 0x06, 'J', 'F', 'I', 'F'}; d.insert(d.end(), a0, a0 + 8); }
    std::vector<unsigned char> t;
    auto u16 = [&](unsigned v) { if (little) { t.push_back(v & 0xFF); t.push_back(v >> 8); } else { t.push_back(v >> 8); t.push_back(v & 0xFF); } };
    auto u32 = [&](unsigned v) { if (little) for (int k = 0; k < 4; ++k) t.push_back((v >> (8 * k)) & 0xFF); else for (int k = 3; k >= 0; --k) t.push_back((v >> (8 * k)) & 0xFF); };
    t.push_back(little ? 'I' : 'M'); t.push_back(little ? 'I' : 'M');
    u16(42); u32(8);
    u16(2);                                         // two entries: something else, then orientation
    u16(271); u16(2); u32(4); u32(0);               // Make (ASCII), ignored
    u16(274); u16(3); u32(1); u16((unsigned)o); u16(0);
    u32(0);
    const std::size_t len = 2 + 6 + t.size();
    d.push_back(0xFF); d.push_back(0xE1); d.push_back((unsigned char)(len >> 8)); d.push_back((unsigned char)(len & 0xFF));
    unsigned char ex[] = {'E', 'x', 'i', 'f', 0, 0};
    d.insert(d.end(), ex, ex + 6);
    d.insert(d.end(), t.begin(), t.end());
    unsigned char sos[] = {0xFF, 0xDA, 0x00, 0x02, 0x12, 0x34};
    d.insert(d.end(), sos, sos + 6);
    return d;
}

static void testExif() {
    for (int o = 1; o <= 8; ++o) {
        auto le = jpegWithOrientation(o, true), be = jpegWithOrientation(o, false, false);
        CHECK(jpegOrientation(le.data(), le.size()) == o);
        CHECK(jpegOrientation(be.data(), be.size()) == o);
    }
    // Not a JPEG, empty, no EXIF, an out-of-range value.
    const unsigned char png[] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    CHECK(jpegOrientation(png, sizeof png) == 1);
    CHECK(jpegOrientation(nullptr, 0) == 1);
    const unsigned char bare[] = {0xFF, 0xD8, 0xFF, 0xDA, 0x00, 0x02};
    CHECK(jpegOrientation(bare, sizeof bare) == 1);
    auto bad = jpegWithOrientation(9, true);
    CHECK(jpegOrientation(bad.data(), bad.size()) == 1);
    // Cut short at every length: never reads past the end, never guesses.
    auto full = jpegWithOrientation(6, true);
    int found = 0;
    for (std::size_t cut = 0; cut <= full.size(); ++cut) {
        std::vector<unsigned char> part(full.begin(), full.begin() + (long)cut);
        const int o = jpegOrientation(part.data(), part.size());
        CHECK(o == 1 || o == 6);
        if (o == 6) ++found;
    }
    CHECK(found > 0);
    // Lengths and offsets that point nowhere.
    auto lie = jpegWithOrientation(6, true);
    lie[20 + 4] = 0xFF; lie[20 + 5] = 0xFF; lie[20 + 6] = 0xFF; lie[20 + 7] = 0x7F;   // IFD offset far away
    const int o = jpegOrientation(lie.data(), lie.size());
    CHECK(o == 1 || o == 6);

    // Turning pixels: a 3x2 picture with a different value in each pixel.
    auto pic = [] {
        std::vector<unsigned char> p;
        for (int i = 0; i < 6; ++i) for (int k = 0; k < 4; ++k) p.push_back((unsigned char)(i * 10 + k));
        return p;
    };
    auto at = [](const std::vector<unsigned char>& p, int w, int x, int y) { return p[((std::size_t)y * w + x) * 4] / 10; };
    // Stored:  0 1 2
    //          3 4 5
    struct Case { int o, w, h; int expect[6]; };
    const Case cases[] = {
        {1, 3, 2, {0, 1, 2, 3, 4, 5}}, {2, 3, 2, {2, 1, 0, 5, 4, 3}}, {3, 3, 2, {5, 4, 3, 2, 1, 0}}, {4, 3, 2, {3, 4, 5, 0, 1, 2}},
        {5, 2, 3, {0, 3, 1, 4, 2, 5}}, {6, 2, 3, {3, 0, 4, 1, 5, 2}}, {7, 2, 3, {5, 2, 4, 1, 3, 0}}, {8, 2, 3, {2, 5, 1, 4, 0, 3}},
    };
    for (const Case& c : cases) {
        auto p = pic();
        int w = 3, h = 2;
        applyOrientation(p, w, h, c.o);
        CHECK(w == c.w && h == c.h);
        bool same = true;
        for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) if (at(p, w, x, y) != c.expect[y * w + x]) same = false;
        CHECK(same);
        CHECK(p[1] % 10 == 1);   // channels stay together
    }
    std::vector<unsigned char> wrong(5);
    int ww = 3, wh = 2;
    applyOrientation(wrong, ww, wh, 6);   // the wrong size: left alone
    CHECK(ww == 3 && wh == 2 && wrong.size() == 5);
}

static void testOperations() {
    std::printf("operations\n");
    CHECK_EQ(uniqueName(L"New folder", {L"a.txt"}, true), L"New folder");
    CHECK_EQ(uniqueName(L"New folder", {L"new FOLDER", L"New folder (2)"}, true), L"New folder (3)");
    CHECK_EQ(uniqueName(L"Notes.txt", {L"notes.TXT"}, false), L"Notes (2).txt");
    CHECK_EQ(uniqueName(L"v1.2", {L"v1.2"}, true), L"v1.2 (2)");        // a folder's dot isn't an extension
    CHECK_EQ(uniqueName(L".gitignore", {L".gitignore"}, false), L".gitignore (2)");
    CHECK_EQ(copyName(L"cover.png", {L"cover.png"}, false), L"cover - Copy.png");
    CHECK_EQ(copyName(L"cover.png", {L"cover.png", L"COVER - copy.png"}, false), L"cover - Copy (2).png");
    CHECK_EQ(copyName(L"Sketches", {L"Sketches"}, true), L"Sketches - Copy");
    CHECK_EQ(renameProblem(L"a.txt", L"b.txt", {L"a.txt", L"c.txt"}), L"");
    CHECK_EQ(renameProblem(L"a.txt", L"A.TXT", {L"a.txt"}), L"");            // only the capitals change
    CHECK(!renameProblem(L"a.txt", L"C.txt", {L"a.txt", L"c.txt"}).empty());
    CHECK(!renameProblem(L"a", L"", {}).empty());
    CHECK(!renameProblem(L"a", L"b:c", {}).empty());
    CHECK(!renameProblem(L"a", L"ends with dot.", {}).empty());
    CHECK(!renameProblem(L"a", L"con.txt", {}).empty());
    CHECK(!renameProblem(L"a", L"LPT1", {}).empty());
    CHECK(renameProblem(L"a", L"console.txt", {}).empty());
    CHECK(isInside(L"C:\\A\\B", L"C:\\a"));
    CHECK(isInside(L"C:\\A", L"C:\\A"));
    CHECK(!isInside(L"C:\\AB", L"C:\\A"));
    CHECK(isInside(L"C:\\x", L"C:\\"));
    {
        auto p = planTransfer({L"C:\\Art\\Sketches", L"C:\\Art\\cover.png"}, L"C:\\Art\\Sketches\\Old", false);
        CHECK(p.sources.size() == 1 && p.sources[0] == L"C:\\Art\\cover.png" && !p.problem.empty());
    }
    {
        auto p = planTransfer({L"C:\\Art\\cover.png"}, L"C:\\art", true);   // moving where it already is
        CHECK(p.sources.empty() && p.nothingToDo && p.problem.empty());
        auto q = planTransfer({L"C:\\Art\\cover.png"}, L"C:\\Art", false);   // copying there makes a copy
        CHECK(q.sources.size() == 1 && !q.nothingToDo);
    }
}


static void testSearch() {
    std::printf("search\n");
    using namespace nrf::search;
    Query q = parse(L"Cover  Sketch");
    CHECK(q.words.size() == 2);
    CHECK(matches(q, L"old sketch for the COVER.png", false));
    CHECK(!matches(q, L"cover.png", false));
    q = parse(L"\"dragon map\" final");
    CHECK(q.words.size() == 2);
    CHECK_EQ(q.words[0], L"dragon map");
    CHECK(matches(q, L"Dragon Map (final).psd", false));
    CHECK(!matches(q, L"map of the dragon final.psd", false));
    q = parse(L"*.png");
    CHECK(q.patterns.size() == 1 && q.words.empty());
    CHECK(matches(q, L"Cover.PNG", false));
    CHECK(!matches(q, L"cover.png.txt", false));
    CHECK(wildcardMatch(L"ch?pter*", L"Chapter 12.docx"));
    CHECK(!wildcardMatch(L"ch?pter*", L"Chpter 12.docx"));
    CHECK(wildcardMatch(L"*a*b*c*", L"xxaxxbxxcxx"));
    CHECK(!wildcardMatch(L"*a*b*c", L"xxaxxbxxcxx"));
    CHECK(wildcardMatch(L"*", L""));
    q = parse(L"kind:picture tree");
    CHECK(q.kind == Kind::Picture);
    CHECK(matches(q, L"Tree study.psd", false));
    CHECK(matches(q, L"tree.procreate", false));
    CHECK(!matches(q, L"tree notes.txt", false));
    CHECK(!matches(q, L"Trees", true));
    q = parse(L"type:folder");
    CHECK(matches(q, L"Anything", true) && !matches(q, L"Anything.txt", false));
    q = parse(L"ext:PSD");
    CHECK(q.exts.size() == 1 && q.exts[0] == L".psd");
    CHECK(matches(q, L"a.Psd", false) && !matches(q, L"a.png", false));
    q = parse(L".kra");
    CHECK(q.exts.size() == 1 && matches(q, L"study.kra", false));
    q = parse(L"kind:wizard");
    CHECK(q.kind == Kind::Any && q.words.size() == 1);
    CHECK(parse(L"   ").empty());
    CHECK(parse(L"*").empty());
    CHECK(kindOf(L".MP4") == Kind::Video && kindOf(L".flac") == Kind::Music && kindOf(L".epub") == Kind::Document && kindOf(L".xyz") == Kind::Any);

    const std::wstring sql = indexSql(parse(L"o'brien 50% *.png kind:picture"), L"C:\\Users\\CJ", 500);
    CHECK(sql.find(L"SELECT TOP 500 System.ItemPathDisplay") == 0);
    CHECK(sql.find(L"SCOPE='file:C:\\Users\\CJ'") != std::wstring::npos);
    CHECK(sql.find(L"System.FileName LIKE '%o''brien%'") != std::wstring::npos);
    CHECK(sql.find(L"System.FileName LIKE '%50[%]%'") != std::wstring::npos);
    CHECK(sql.find(L"System.FileName LIKE '%.png'") != std::wstring::npos);
    CHECK(sql.find(L"System.Kind = 'picture'") != std::wstring::npos);
    CHECK(indexSql(parse(L"x"), L"", 10).find(L"SCOPE='file:'") != std::wstring::npos);
    CHECK(indexSql(parse(L"ext:psd ext:kra"), L"", 10).find(L"(System.FileExtension = '.psd' OR System.FileExtension = '.kra')") != std::wstring::npos);

    CHECK_EQ(relativeFolder(L"C:\\Art", L"C:\\Art\\Sketches\\Old"), L"Sketches\\Old");
    CHECK_EQ(relativeFolder(L"C:\\Art", L"c:\\art\\x"), L"x");
    CHECK_EQ(relativeFolder(L"C:\\Art", L"C:\\Art"), L"");
    CHECK_EQ(relativeFolder(L"C:\\", L"C:\\Users"), L"Users");
    CHECK_EQ(relativeFolder(L"C:\\Art", L"D:\\Other\\Place"), L"D:\\Other\\Place");
}


// A small Photoshop file: 4x2 RGB, 8 bits, with a preview resource and the flattened picture.
static std::vector<unsigned char> makePsd(bool rle, bool withPreview) {
    std::vector<unsigned char> f;
    auto be = [&](unsigned long long v, int bytes) { for (int i = bytes - 1; i >= 0; --i) f.push_back((unsigned char)(v >> (8 * i))); };
    for (char c : std::string("8BPS")) f.push_back((unsigned char)c);
    be(1, 2); be(0, 6); be(3, 2); be(2, 4); be(4, 4); be(8, 2); be(3, 2);
    be(0, 4);   // colour mode data
    std::vector<unsigned char> res;
    if (withPreview) {
        const unsigned char jpeg[] = {0xFF, 0xD8, 0xFF, 0xE0, 1, 2, 3, 0xFF, 0xD9};
        auto rbe = [&](unsigned long long v, int bytes) { for (int i = bytes - 1; i >= 0; --i) res.push_back((unsigned char)(v >> (8 * i))); };
        for (char c : std::string("8BIM")) res.push_back((unsigned char)c);
        rbe(1005, 2); rbe(0, 2); rbe(4, 4); rbe(7, 4);   // another resource first
        for (char c : std::string("8BIM")) res.push_back((unsigned char)c);
        rbe(1036, 2); rbe(0, 2); rbe(28 + sizeof jpeg, 4);
        for (int i = 0; i < 28; ++i) res.push_back(0);
        for (unsigned char b : jpeg) res.push_back(b);
        if (res.size() % 2) res.push_back(0);
    }
    be(res.size(), 4);
    f.insert(f.end(), res.begin(), res.end());
    be(0, 4);   // no layers
    // Channels: R, G, B; 2 rows of 4.
    const unsigned char R[8] = {255, 255, 0, 0, 10, 10, 10, 10}, G[8] = {0, 0, 255, 255, 20, 20, 20, 20}, B[8] = {0, 0, 0, 0, 30, 30, 30, 30};
    const unsigned char* ch[3] = {R, G, B};
    if (!rle) {
        be(0, 2);
        for (auto* c : ch) for (int i = 0; i < 8; ++i) f.push_back(c[i]);
    } else {
        be(1, 2);
        // Each row as PackBits: a literal pair then a run of two (first row), one run of four (second).
        std::vector<std::vector<unsigned char>> rows;
        for (auto* c : ch) {
            rows.push_back({1, c[0], c[1], (unsigned char)(signed char)-1, c[2]});
            rows.push_back({(unsigned char)(signed char)-3, c[4]});
        }
        for (auto& r : rows) be(r.size(), 2);
        for (auto& r : rows) f.insert(f.end(), r.begin(), r.end());
    }
    return f;
}

static void testArt() {
    std::printf("art files\n");
    using namespace nrf::art;
    for (bool rle : {false, true}) {
        const auto f = makePsd(rle, true);
        const PsdInfo info = psdInfo(f.data(), f.size());
        CHECK(info.ok && info.width == 4 && info.height == 2 && info.channels == 3 && info.mode == 3 && info.imageDataStart > 0);
        Image img;
        CHECK(psdComposite(f.data(), f.size(), 100, img));
        CHECK(img.w == 4 && img.h == 2);
        if (img.w == 4 && img.h == 2) {
            CHECK(img.rgba[0] == 255 && img.rgba[1] == 0 && img.rgba[2] == 0 && img.rgba[3] == 255);    // red
            CHECK(img.rgba[8] == 0 && img.rgba[9] == 255 && img.rgba[10] == 0);                          // green
            CHECK(img.rgba[16] == 10 && img.rgba[17] == 20 && img.rgba[18] == 30);                       // second row
        }
        Image small;
        CHECK(psdComposite(f.data(), f.size(), 2, small));
        CHECK(small.w == 2 && small.h == 1);
        if (small.w == 2) CHECK(small.rgba[0] == (255 + 255 + 10 + 10) / 4 && small.rgba[1] == 10);   // averaged
    }
    const auto f = makePsd(false, true);
    const std::string jpeg = psdPreviewJpeg(f.data(), f.size());
    CHECK(jpeg.size() == 9 && (unsigned char)jpeg[0] == 0xFF && (unsigned char)jpeg[1] == 0xD8);
    // Just the start of the file is enough for the preview, not for the picture.
    CHECK(psdPreviewJpeg(f.data(), 104).size() == 9);
    Image none;
    CHECK(!psdComposite(f.data(), 104, 100, none));
    const auto g = makePsd(true, false);
    CHECK(psdPreviewJpeg(g.data(), g.size()).empty());
    CHECK(!psdInfo((const unsigned char*)"not a psd at all, really not", 28).ok);

    const std::string utf8 = "caf\xC3\xA9\r\nline two\n\tthree\nfour";
    bool cut = false;
    std::wstring t = decodeText((const unsigned char*)utf8.data(), utf8.size(), 3, &cut);
    CHECK_EQ(t, L"café\nline two\n    three");
    CHECK(cut);
    const std::string ansi = "na\xEFve \x93quoted\x94";
    CHECK_EQ(decodeText((const unsigned char*)ansi.data(), ansi.size(), 10), L"naïve “quoted”");
    const unsigned char u16[] = {0xFF, 0xFE, 'h', 0, 'i', 0};
    CHECK_EQ(decodeText(u16, sizeof u16, 10), L"hi");
    CHECK(looksLikeText((const unsigned char*)utf8.data(), utf8.size()));
    const unsigned char bin[] = {'P', 'K', 3, 4, 0, 0, 1, 2};
    CHECK(!looksLikeText(bin, sizeof bin));
}

int main() {
    testNormalize();
    testPathParts();
    testSorting();
    testSizes();
    testSelection();
    testExif();
    testOperations();
    testSearch();
    testArt();
    std::printf("\n%d passed, %d failed\n", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#include "nrf/search.hpp"

#include "nrf/paths.hpp"

#include <algorithm>
#include <cwctype>

namespace nrf::search {

namespace {

std::wstring lower(std::wstring s) {
    for (auto& c : s) c = (wchar_t)towlower(c);
    return s;
}

bool startsWith(const std::wstring& s, const wchar_t* p) {
    const std::wstring pre = p;
    return s.size() > pre.size() && s.compare(0, pre.size(), pre) == 0;
}

// A value inside '...' in Windows Search SQL.
std::wstring quoted(const std::wstring& s) {
    std::wstring o = L"'";
    for (wchar_t c : s) {
        if (c == L'\'') o += L"''";
        else o += c;
    }
    return o + L"'";
}

// Characters LIKE treats specially, made literal.
std::wstring likeLiteral(const std::wstring& s) {
    std::wstring o;
    for (wchar_t c : s) {
        if (c == L'%' || c == L'_' || c == L'[') { o += L'['; o += c; o += L']'; }
        else o += c;
    }
    return o;
}

}  // namespace

Kind kindOf(const std::wstring& extIn) {
    static const wchar_t* pictures[] = {L".jpg", L".jpeg", L".png", L".gif", L".bmp", L".tif", L".tiff", L".webp", L".heic", L".heif",
                                        L".avif", L".ico", L".svg", L".psd", L".procreate", L".kra", L".clip", L".xcf", L".raw",
                                        L".cr2", L".cr3", L".nef", L".arw", L".dng", L".jfif"};
    static const wchar_t* videos[] = {L".mp4", L".mkv", L".mov", L".avi", L".wmv", L".webm", L".m4v", L".flv", L".mpg", L".mpeg", L".3gp", L".ts"};
    static const wchar_t* music[] = {L".mp3", L".wav", L".flac", L".m4a", L".aac", L".ogg", L".opus", L".wma", L".aiff", L".mid", L".midi"};
    static const wchar_t* documents[] = {L".txt", L".md", L".rtf", L".doc", L".docx", L".odt", L".pdf", L".xls", L".xlsx", L".ods",
                                         L".csv", L".ppt", L".pptx", L".odp", L".epub", L".fdx", L".fountain", L".html", L".htm"};
    static const wchar_t* archives[] = {L".zip", L".7z", L".rar", L".tar", L".gz", L".bz2", L".xz", L".cab", L".iso"};
    const std::wstring ext = lower(extIn);
    auto in = [&](const auto& list) { return std::any_of(std::begin(list), std::end(list), [&](const wchar_t* x) { return ext == x; }); };
    if (in(pictures)) return Kind::Picture;
    if (in(videos)) return Kind::Video;
    if (in(music)) return Kind::Music;
    if (in(documents)) return Kind::Document;
    if (in(archives)) return Kind::Archive;
    return Kind::Any;
}

Query parse(const std::wstring& text) {
    Query q;
    std::vector<std::wstring> tokens;
    std::wstring cur;
    bool inQuotes = false;
    for (wchar_t c : text) {
        if (c == L'"') {
            if (inQuotes) { if (!cur.empty()) tokens.push_back(L"\"" + cur); cur.clear(); }
            else if (!cur.empty()) { tokens.push_back(cur); cur.clear(); }
            inQuotes = !inQuotes;
        } else if (!inQuotes && iswspace(c)) {
            if (!cur.empty()) { tokens.push_back(cur); cur.clear(); }
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) tokens.push_back(inQuotes ? L"\"" + cur : cur);
    for (std::wstring t : tokens) {
        if (t[0] == L'"') {   // a phrase: the words together, as typed
            t = lower(t.substr(1));
            if (!t.empty()) q.words.push_back(t);
            continue;
        }
        const std::wstring l = lower(t);
        if (startsWith(l, L"kind:") || startsWith(l, L"type:")) {
            const std::wstring k = l.substr(5);
            if (k == L"folder" || k == L"folders") q.kind = Kind::Folder;
            else if (k == L"picture" || k == L"pictures" || k == L"image" || k == L"images" || k == L"photo" || k == L"photos") q.kind = Kind::Picture;
            else if (k == L"video" || k == L"videos" || k == L"movie" || k == L"movies") q.kind = Kind::Video;
            else if (k == L"music" || k == L"audio" || k == L"song" || k == L"songs") q.kind = Kind::Music;
            else if (k == L"document" || k == L"documents" || k == L"doc" || k == L"docs" || k == L"text") q.kind = Kind::Document;
            else if (k == L"archive" || k == L"archives" || k == L"zip") q.kind = Kind::Archive;
            else q.words.push_back(l);   // not a kind we know: look for it in names
            continue;
        }
        if (startsWith(l, L"ext:")) {
            std::wstring e = l.substr(4);
            if (!e.empty() && e[0] != L'.') e = L"." + e;
            if (e.size() > 1) q.exts.push_back(e);
            continue;
        }
        if (l.find_first_of(L"*?") != std::wstring::npos) {
            if (l != L"*") q.patterns.push_back(l);
            continue;
        }
        if (l.size() > 1 && l[0] == L'.' && l.find(L'.', 1) == std::wstring::npos && l.find_first_of(L"\\/") == std::wstring::npos) {
            q.exts.push_back(l);   // ".psd": that kind of file
            continue;
        }
        q.words.push_back(l);
    }
    return q;
}

bool wildcardMatch(const std::wstring& pattern, const std::wstring& name) {
    // Iterative, with one backtrack point per star: linear-ish, never exponential.
    std::size_t p = 0, n = 0, star = std::wstring::npos, mark = 0;
    while (n < name.size()) {
        if (p < pattern.size() && (pattern[p] == L'?' || towlower(pattern[p]) == towlower(name[n]))) { ++p; ++n; }
        else if (p < pattern.size() && pattern[p] == L'*') { star = p++; mark = n; }
        else if (star != std::wstring::npos) { p = star + 1; n = ++mark; }
        else return false;
    }
    while (p < pattern.size() && pattern[p] == L'*') ++p;
    return p == pattern.size();
}

bool matches(const Query& q, const std::wstring& name, bool folder) {
    if (q.empty()) return true;
    const std::wstring l = lower(name);
    for (const auto& w : q.words)
        if (l.find(w) == std::wstring::npos) return false;
    for (const auto& p : q.patterns)
        if (!wildcardMatch(p, l)) return false;
    const std::wstring ext = folder ? L"" : extensionOf(name);
    if (!q.exts.empty() && std::find(q.exts.begin(), q.exts.end(), ext) == q.exts.end()) return false;
    switch (q.kind) {
        case Kind::Any: return true;
        case Kind::Folder: return folder;
        default: return !folder && kindOf(ext) == q.kind;
    }
}

std::wstring indexSql(const Query& q, const std::wstring& scope, int limit) {
    std::wstring sql = L"SELECT TOP " + std::to_wstring(std::max(1, limit)) +
                       L" System.ItemPathDisplay, System.Size, System.DateModified, System.FileAttributes FROM SystemIndex WHERE ";
    std::vector<std::wstring> where;
    where.push_back(L"SCOPE=" + quoted(L"file:" + scope));
    for (const auto& w : q.words) where.push_back(L"System.FileName LIKE " + quoted(L"%" + likeLiteral(w) + L"%"));
    for (const auto& p : q.patterns) {
        std::wstring like;
        for (wchar_t c : p) {
            if (c == L'*') like += L'%';
            else if (c == L'?') like += L'_';
            else like += likeLiteral(std::wstring(1, c));
        }
        where.push_back(L"System.FileName LIKE " + quoted(like));
    }
    if (!q.exts.empty()) {
        std::wstring any = L"(";
        for (std::size_t i = 0; i < q.exts.size(); ++i) any += (i ? L" OR " : L"") + std::wstring(L"System.FileExtension = ") + quoted(q.exts[i]);
        where.push_back(any + L")");
    }
    switch (q.kind) {
        case Kind::Folder: where.push_back(L"System.Kind = 'folder'"); break;
        case Kind::Picture: where.push_back(L"System.Kind = 'picture'"); break;
        case Kind::Video: where.push_back(L"System.Kind = 'video'"); break;
        case Kind::Music: where.push_back(L"System.Kind = 'music'"); break;
        case Kind::Document: where.push_back(L"System.Kind = 'document'"); break;
        case Kind::Archive: {
            std::wstring any = L"(";
            const wchar_t* list[] = {L".zip", L".7z", L".rar", L".tar", L".gz", L".bz2", L".xz", L".cab", L".iso"};
            for (std::size_t i = 0; i < std::size(list); ++i) any += (i ? L" OR " : L"") + std::wstring(L"System.FileExtension = ") + quoted(list[i]);
            where.push_back(any + L")");
            break;
        }
        case Kind::Any: break;
    }
    for (std::size_t i = 0; i < where.size(); ++i) sql += (i ? L" AND " : L"") + where[i];
    return sql;
}

std::wstring relativeFolder(const std::wstring& root, const std::wstring& folder) {
    if (folder.size() <= root.size()) return L"";
    if (lower(folder.substr(0, root.size())) != lower(root)) return folder;
    std::wstring rest = folder.substr(root.size());
    while (!rest.empty() && rest[0] == L'\\') rest.erase(0, 1);
    return rest;
}

}  // namespace nrf::search

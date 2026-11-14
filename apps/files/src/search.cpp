#include "search.hpp"

#include "nrf/paths.hpp"
#include "nrf/search.hpp"

#include <oledb.h>
#include <msdasc.h>
#include <oledberr.h>

#include <cstddef>
#include <deque>
#include <thread>

namespace nrf {

namespace {

// OLE DB's ids (MinGW's headers declare them without definitions).
const CLSID kClsidDataInitialize = {0x2206CDB0, 0x19C1, 0x11D1, {0x89, 0xE0, 0x00, 0xC0, 0x4F, 0xD7, 0xA8, 0x29}};
const IID kIidDataInitialize = {0x2206CCB1, 0x19C1, 0x11D1, {0x89, 0xE0, 0x00, 0xC0, 0x4F, 0xD7, 0xA8, 0x29}};
const IID kIidDBInitialize = {0x0c733a8b, 0x2a1c, 0x11ce, {0xad, 0xe5, 0x00, 0xaa, 0x00, 0x44, 0x77, 0x3d}};
const IID kIidDBCreateSession = {0x0c733a5d, 0x2a1c, 0x11ce, {0xad, 0xe5, 0x00, 0xaa, 0x00, 0x44, 0x77, 0x3d}};
const IID kIidDBCreateCommand = {0x0c733a1d, 0x2a1c, 0x11ce, {0xad, 0xe5, 0x00, 0xaa, 0x00, 0x44, 0x77, 0x3d}};
const IID kIidCommandText = {0x0c733a27, 0x2a1c, 0x11ce, {0xad, 0xe5, 0x00, 0xaa, 0x00, 0x44, 0x77, 0x3d}};
const IID kIidRowset = {0x0c733a7c, 0x2a1c, 0x11ce, {0xad, 0xe5, 0x00, 0xaa, 0x00, 0x44, 0x77, 0x3d}};
const IID kIidAccessor = {0x0c733a8c, 0x2a1c, 0x11ce, {0xad, 0xe5, 0x00, 0xaa, 0x00, 0x44, 0x77, 0x3d}};
const GUID kDbguidDefault = {0xc8b521fb, 0x5cf3, 0x11ce, {0xad, 0xe5, 0x00, 0xaa, 0x00, 0x44, 0x77, 0x3d}};

template <class T>
struct Com {
    T* p = nullptr;
    ~Com() { if (p) p->Release(); }
    T** put() { return &p; }
    T* operator->() const { return p; }
    explicit operator bool() const { return p != nullptr; }
};

// Sends batches as results are found: every so often, so they appear as they're found.
struct Sender {
    HWND window;
    std::uint64_t request;
    bool noIndex = false;
    ListBatch* batch = nullptr;
    DWORD lastSend = GetTickCount();
    std::size_t total = 0;
    std::uint64_t folders = 0;
    Sender(HWND w, std::uint64_t r) : window(w), request(r) { fresh(); }
    ~Sender() { delete batch; }
    void fresh() {
        batch = new ListBatch;
        batch->request = request;
        batch->noIndex = noIndex;
    }
    void add(Entry e) {
        batch->entries.push_back(std::move(e));
        ++total;
    }
    void maybeSend() {
        if (batch->entries.size() >= 2000 || (GetTickCount() - lastSend > 120 && (!batch->entries.empty() || GetTickCount() - lastSend > 600))) send(false);
    }
    void send(bool done) {
        ListBatch* b = batch;
        b->done = done;
        b->noIndex = noIndex;
        b->foldersRead = folders;
        postToWindow(window, WM_APP_LIST, b, [b] { delete b; });
        batch = nullptr;
        if (!done) fresh();
        lastSend = GetTickCount();
    }
};

// Reads the folders, breadth first (what's near the top shows first).
void walk(const std::vector<std::wstring>& roots, const search::Query& q, const Visibility& vis, Sender& out,
          const std::function<bool()>& stale) {
    std::deque<std::wstring> todo(roots.begin(), roots.end());
    while (!todo.empty()) {
        if (stale()) return;
        const std::wstring folder = todo.front();
        todo.pop_front();
        ++out.folders;
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileExW((extendedPath(folder) + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr,
                                    FIND_FIRST_EX_LARGE_FETCH);
        if (h == INVALID_HANDLE_VALUE) continue;   // can't be read (no permission, say): the rest still can
        do {
            const std::wstring name = fd.cFileName;
            if (name == L"." || name == L"..") continue;
            Entry e;
            e.name = name;
            e.attributes = fd.dwFileAttributes;
            const bool dir = (e.attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            // Into sub-folders that would be shown, but not through links (they can loop).
            if (dir && !e.link() && isVisible(e, vis)) todo.push_back(join(folder, name));
            if (!search::matches(q, name, dir)) continue;
            e.size = dir ? 0 : ((std::uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
            e.modified = ((std::uint64_t)fd.ftLastWriteTime.dwHighDateTime << 32) | fd.ftLastWriteTime.dwLowDateTime;
            if (e.attributes & FILE_ATTRIBUTE_REPARSE_POINT) e.reparseTag = fd.dwReserved0;
            e.location = folder;
            describeEntry(e, join(folder, name));
            out.add(std::move(e));
            if (out.total >= kMostSearchResults) {
                FindClose(h);
                out.batch->limited = true;
                return;
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
        out.maybeSend();
    }
}

// The Windows Search index: false if it couldn't be asked at all.
bool askIndex(const std::wstring& sql, const Visibility& vis, Sender& out, const std::function<bool()>& stale) {
    Com<IDataInitialize> init;
    if (FAILED(CoCreateInstance(kClsidDataInitialize, nullptr, CLSCTX_INPROC_SERVER, kIidDataInitialize, reinterpret_cast<void**>(init.put()))))
        return false;
    Com<IUnknown> source;
    wchar_t conn[] = L"Provider=Search.CollatorDSO;Extended Properties='Application=Windows';";
    if (FAILED(init->GetDataSource(nullptr, CLSCTX_INPROC_SERVER, conn, kIidDBInitialize, source.put()))) return false;
    Com<IDBInitialize> dbInit;
    if (FAILED(source->QueryInterface(kIidDBInitialize, reinterpret_cast<void**>(dbInit.put()))) || FAILED(dbInit->Initialize())) return false;
    Com<IDBCreateSession> sessions;
    if (FAILED(dbInit->QueryInterface(kIidDBCreateSession, reinterpret_cast<void**>(sessions.put())))) return false;
    Com<IDBCreateCommand> commands;
    if (FAILED(sessions->CreateSession(nullptr, kIidDBCreateCommand, reinterpret_cast<IUnknown**>(commands.put())))) return false;
    Com<ICommandText> command;
    if (FAILED(commands->CreateCommand(nullptr, kIidCommandText, reinterpret_cast<IUnknown**>(command.put())))) return false;
    if (FAILED(command->SetCommandText(kDbguidDefault, sql.c_str()))) return false;
    Com<IRowset> rows;
    if (FAILED(command->Execute(nullptr, kIidRowset, nullptr, nullptr, reinterpret_cast<IUnknown**>(rows.put()))) || !rows) return false;

    struct Row {
        DBSTATUS pathStatus;
        DBLENGTH pathLength;
        wchar_t path[2048];
        DBSTATUS sizeStatus;
        ULONGLONG size;
        DBSTATUS dateStatus;
        FILETIME date;
        DBSTATUS attrStatus;
        ULONG attrs;
    };
    DBBINDING b[4] = {};
    auto bind = [&](int i, DBORDINAL col, std::size_t value, std::size_t status, std::size_t length, DBTYPE type, DBLENGTH max) {
        b[i].iOrdinal = col;
        b[i].obValue = value;
        b[i].obStatus = status;
        b[i].obLength = length;
        b[i].dwPart = DBPART_VALUE | DBPART_STATUS | (length != (std::size_t)-1 ? DBPART_LENGTH : 0);
        if (length == (std::size_t)-1) b[i].obLength = 0;
        b[i].dwMemOwner = DBMEMOWNER_CLIENTOWNED;
        b[i].eParamIO = DBPARAMIO_NOTPARAM;
        b[i].cbMaxLen = max;
        b[i].wType = type;
    };
    bind(0, 1, offsetof(Row, path), offsetof(Row, pathStatus), offsetof(Row, pathLength), DBTYPE_WSTR, sizeof(Row::path));
    bind(1, 2, offsetof(Row, size), offsetof(Row, sizeStatus), (std::size_t)-1, DBTYPE_UI8, sizeof(ULONGLONG));
    bind(2, 3, offsetof(Row, date), offsetof(Row, dateStatus), (std::size_t)-1, DBTYPE_FILETIME, sizeof(FILETIME));
    bind(3, 4, offsetof(Row, attrs), offsetof(Row, attrStatus), (std::size_t)-1, DBTYPE_UI4, sizeof(ULONG));
    Com<IAccessor> accessor;
    if (FAILED(rows->QueryInterface(kIidAccessor, reinterpret_cast<void**>(accessor.put())))) return false;
    HACCESSOR acc = 0;
    DBBINDSTATUS bs[4] = {};
    if (FAILED(accessor->CreateAccessor(DBACCESSOR_ROWDATA, 4, b, sizeof(Row), &acc, bs))) return false;

    Row row{};
    HROW handles[64];
    for (;;) {
        if (stale()) break;
        DBCOUNTITEM got = 0;
        HROW* ph = handles;
        const HRESULT hr = rows->GetNextRows(DB_NULL_HCHAPTER, 0, 64, &got, &ph);
        for (DBCOUNTITEM i = 0; i < got; ++i) {
            row = Row{};
            if (FAILED(rows->GetData(handles[i], acc, &row)) || row.pathStatus != DBSTATUS_S_OK) continue;
            const std::wstring path(row.path, wcsnlen(row.path, std::size(row.path)));
            if (path.size() < 3) continue;
            Entry e;
            e.name = nameOf(path);
            e.location = parentOf(path);
            if (e.name.empty() || e.location.empty()) continue;
            e.attributes = row.attrStatus == DBSTATUS_S_OK ? row.attrs : 0;
            if (row.attrStatus != DBSTATUS_S_OK) {
                const DWORD a = GetFileAttributesW(extendedPath(path).c_str());
                if (a != INVALID_FILE_ATTRIBUTES) e.attributes = a;
            }
            if (!isVisible(e, vis)) continue;
            e.size = (row.sizeStatus == DBSTATUS_S_OK && !e.folder()) ? row.size : 0;
            if (row.dateStatus == DBSTATUS_S_OK) e.modified = ((std::uint64_t)row.date.dwHighDateTime << 32) | row.date.dwLowDateTime;
            describeEntry(e, path);
            out.add(std::move(e));
        }
        if (got) rows->ReleaseRows(got, handles, nullptr, nullptr, nullptr);
        out.maybeSend();
        if (out.total >= kMostSearchResults) { out.batch->limited = true; break; }
        if (hr == DB_S_ENDOFROWSET || got == 0 || FAILED(hr)) break;
    }
    accessor->ReleaseAccessor(acc, nullptr);
    return true;
}

}  // namespace

void searchFoldersAsync(HWND window, std::vector<std::wstring> roots, std::wstring query, Visibility visibility, std::uint64_t request,
                        std::shared_ptr<std::atomic<std::uint64_t>> latest) {
    std::thread([=] {
        const HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        {
            Sender out(window, request);
            auto stale = [&] { return latest->load() != request; };
            walk(roots, search::parse(query), visibility, out, stale);
            if (!stale()) out.send(true);
        }
        if (SUCCEEDED(co)) CoUninitialize();
    }).detach();
}

void searchIndexAsync(HWND window, std::wstring scope, std::wstring query, Visibility visibility, std::vector<std::wstring> fallbackRoots,
                      std::uint64_t request, std::shared_ptr<std::atomic<std::uint64_t>> latest) {
    std::thread([=] {
        const HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        {
            Sender out(window, request);
            auto stale = [&] { return latest->load() != request; };
            const search::Query q = search::parse(query);
            if (!askIndex(search::indexSql(q, scope, (int)kMostSearchResults), visibility, out, stale)) {
                // No index to ask (the Windows Search service is off, or this isn't a PC that has it).
                out.noIndex = true;
                out.batch->noIndex = true;
                walk(fallbackRoots, q, visibility, out, stale);
            }
            if (!stale()) out.send(true);
        }
        if (SUCCEEDED(co)) CoUninitialize();
    }).detach();
}

}  // namespace nrf

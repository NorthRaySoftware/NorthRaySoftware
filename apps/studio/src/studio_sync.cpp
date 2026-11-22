// Worldbuilding › Sync with GitHub (0.61): a world kept in the writer's own GitHub repository, so it can be written on
// more than one computer. The work is in studiocore (studio/sync.hpp); this is the page, the token's keeping (Windows
// Credential Manager), the background thread and when syncs happen: on opening the library, every few minutes while
// something has changed (and every ten minutes anyway, for the other computer's changes), and on closing.
#include "studio_app.hpp"
#include "cjui/dialog.hpp"
#include "cjui/http.hpp"

#include <wincred.h>

#include <algorithm>
#include <cstdlib>

namespace nrs {

using namespace studio;
namespace sy = studio::sync;
using cjui::Align;
using cjui::Font;
using cjui::Rect;

static std::wstring w(const std::string& s) { return widen(s); }
static std::wstring plural(size_t n, const wchar_t* one, const wchar_t* many) { return std::to_wstring(n) + L" " + (n == 1 ? one : many); }

namespace {

constexpr ULONGLONG kQuietGap = 2 * 60 * 1000;     // at most one automatic sync of a world every two minutes
constexpr ULONGLONG kCheckGap = 10 * 60 * 1000;    // and one every ten minutes for the other computer's changes

std::wstring credTarget(const std::string& repo) { return w("NorthRayWritingStudio/github/" + repo); }

bool saveToken(const std::string& repo, const std::string& token) {
    std::wstring t = credTarget(repo);
    std::vector<BYTE> blob(token.begin(), token.end());
    CREDENTIALW c{};
    c.Type = CRED_TYPE_GENERIC;
    c.TargetName = t.data();
    c.CredentialBlobSize = (DWORD)blob.size();
    c.CredentialBlob = blob.data();
    c.Persist = CRED_PERSIST_LOCAL_MACHINE;   // this Windows user on this PC; not roamed
    wchar_t user[] = L"NorthRay Writing Studio";
    c.UserName = user;
    const bool ok = CredWriteW(&c, 0) != FALSE;
    SecureZeroMemory(blob.data(), blob.size());
    return ok;
}

std::string loadToken(const std::string& repo) {
    PCREDENTIALW c = nullptr;
    if (repo.empty() || !CredReadW(credTarget(repo).c_str(), CRED_TYPE_GENERIC, 0, &c) || !c) return {};
    std::string s(reinterpret_cast<const char*>(c->CredentialBlob), c->CredentialBlobSize);
    CredFree(c);
    return s;
}

void removeToken(const std::string& repo) { CredDeleteW(credTarget(repo).c_str(), CRED_TYPE_GENERIC, 0); }

sy::Http githubHttp() {
    return [](const std::string& method, const std::string& url, const std::vector<std::pair<std::string, std::string>>& headers,
              const std::string& body) {
        cjui::HttpResult r = cjui::httpRequest(method, url, headers, body);
        return sy::HttpResponse{r.status, std::move(r.body), narrow(r.error)};
    };
}

std::wstring agoText(std::int64_t when) {
    if (when <= 0) return L"never";
    const std::int64_t d = (std::int64_t)std::time(nullptr) - when;
    if (d < 60) return L"just now";
    if (d < 3600) return std::to_wstring(d / 60) + (d / 60 == 1 ? L" minute ago" : L" minutes ago");
    if (d < 86400) return std::to_wstring(d / 3600) + (d / 3600 == 1 ? L" hour ago" : L" hours ago");
    return std::to_wstring(d / 86400) + (d / 86400 == 1 ? L" day ago" : L" days ago");
}

// Whether the token looks like something GitHub gives out (no spaces, a sensible length).
bool plausibleToken(const std::string& t) {
    return t.size() >= 20 && t.size() <= 400 && t.find_first_of(" \t\r\n") == std::string::npos;
}

}  // namespace

// This computer's side of a sync: each step is done on the window's thread, where the library lives.
class SyncLocal : public sy::Local {
public:
    SyncLocal(StudioApp& app, std::string folder) : app_(app), folder_(std::move(folder)) {}
    std::map<std::string, Library::WorldFile> files(const Id& world) override {
        std::map<std::string, Library::WorldFile> out;
        onMain([&] { if (here()) out = app_.lib_->worldFiles(world); });
        return out;
    }
    bool hasWorld(const Id& world) override {
        bool has = false;
        onMain([&] { has = here() && app_.lib_->world(world); });
        return has;
    }
    std::string worldName(const Id& world) override {
        std::string n;
        onMain([&] { if (here()) if (const World* wd = app_.lib_->world(world)) n = wd->name; });
        return n;
    }
    bool apply(const Id& world, const std::map<std::string, std::optional<std::string>>& changes,
               const std::vector<std::pair<Id, std::string>>& keepVersions, std::string& error) override {
        bool ok = false;
        onMain([&] {
            if (!here()) { error = "The library was changed while syncing."; return; }
            app_.commitFields();
            ok = app_.lib_->applyWorldFiles(world, changes, keepVersions, error);
            app_.loadedFocus_ = {Focus::None, "-", ""};
            // The chapter open in Writing, changed on the other computer: shown as it is now.
            if (ok && !app_.currentNode_.empty()) {
                if (!app_.lib_->node(app_.currentNode_)) app_.currentNode_.clear();
                else
                    for (auto& [p, v] : changes)
                        if (p.find("/nodes/" + app_.currentNode_ + ".json") != std::string::npos) { app_.sendLoad(); break; }
            }
            app_.invalidate();
        });
        return ok;
    }
    void save(const Id& world, const sy::State& state) override {
        onMain([&] { if (here()) sy::saveState(*app_.lib_, world, state); });
    }

private:
    bool here() const { return app_.lib_ && app_.lib_->folder() == folder_; }
    void onMain(const std::function<void()>& fn) { SendMessageW(app_.hwnd(), WM_APP_SYNC_CALL, 0, reinterpret_cast<LPARAM>(&fn)); }
    StudioApp& app_;
    std::string folder_;
};

// ------------------------------------------------------------------------------------------- running

void StudioApp::queueSync(const SyncJob& job) {
    for (auto& q : syncQueue_)
        if (q.world == job.world && q.kind == job.kind && q.repo == job.repo) { q.quiet = q.quiet && job.quiet; return; }   // already waiting
    syncQueue_.push_back(job);
    runNextSync();
}

void StudioApp::runNextSync() {
    if (syncBusy_ || syncQueue_.empty() || !lib_) return;
    if (syncThread_.joinable()) syncThread_.join();
    SyncJob job = syncQueue_.front();
    syncQueue_.pop_front();
    if (job.folder != lib_->folder()) { runNextSync(); return; }
    std::string token = loadToken(job.repo);
    if (token.empty()) {
        if (!job.world.empty()) { syncNote_[job.world] = L"This computer doesn't have the token for " + w(job.repo) + L" yet."; syncFailed_[job.world] = true; }
        invalidate();
        runNextSync();
        return;
    }
    sy::State state = job.world.empty() ? sy::State{} : sy::loadState(*lib_, job.world);
    if (state.repo.empty()) state.repo = job.repo;
    syncBusy_ = true;
    syncCancel_ = false;
    syncRunning_ = job.world.empty() ? Id("-") : job.world;
    if (!job.world.empty()) syncTriedAt_[job.world] = GetTickCount64();
    invalidate();
    syncThread_ = std::thread([this, job, token, state]() mutable {
        auto* res = new SyncResult;
        res->job = job;
        std::string owner, name;
        const size_t slash = job.repo.find('/');
        owner = job.repo.substr(0, slash);
        name = slash == std::string::npos ? std::string() : job.repo.substr(slash + 1);
        sy::GitHubRemote remote(owner, name, token, githubHttp(), state.branch);
        // For testing against a stand-in server on this computer only (never another host).
        if (const char* api = std::getenv("NORTHRAY_GITHUB_API"); api && std::string(api).rfind("http://127.0.0.1:", 0) == 0) remote.apiBase = api;
        SecureZeroMemory(token.data(), token.size());
        if (job.kind == SyncJob::Check) {
            res->ok = remote.info(res->info, res->error);
        } else {
            SyncLocal local(*this, job.folder);
            res->ok = sy::syncWorld(local, job.world, remote, state, res->report, res->error, &syncCancel_);
            if (state.branch.empty()) state.branch = remote.branch();
            res->state = state;
        }
        PostMessageW(hwnd(), WM_APP_SYNC_DONE, 0, reinterpret_cast<LPARAM>(res));
    });
}

void StudioApp::syncFinished(SyncResult* resIn) {
    std::unique_ptr<SyncResult> res(resIn);
    if (syncThread_.joinable()) syncThread_.join();
    syncBusy_ = false;
    syncRunning_.clear();
    const SyncJob& job = res->job;
    const bool sameLibrary = lib_ && lib_->folder() == job.folder;
    if (!sameLibrary) { runNextSync(); return; }
    if (job.kind == SyncJob::Check) {
        if (!res->ok) {
            syncNote_[job.world] = w(res->error);
            syncFailed_[job.world] = true;
            removeToken(job.repo);   // it was only saved for this check
            invalidate();
            runNextSync();
            return;
        }
        if (!res->info.isPrivate && !closing_) {
            const int a = cjui::messageBox(hwnd(),
                                           (L"The repository " + w(job.repo) +
                                            L" is public: anyone could read this world, its books and its bible there.\n\nUse it anyway?")
                                               .c_str(),
                                           L"Sync with GitHub", MB_YESNO | MB_ICONWARNING);
            if (a != IDYES) {
                removeToken(job.repo);
                syncNote_[job.world] = L"Not started: the repository is public. Make it private on GitHub (Settings, Danger zone), or use another.";
                syncFailed_[job.world] = true;
                invalidate();
                runNextSync();
                return;
            }
        }
        if (!job.world.empty()) {
            sy::State s;
            s.repo = job.repo;
            s.branch = res->info.defaultBranch;
            sy::saveState(*lib_, job.world, s);
        }
        SyncJob next = job;
        next.kind = SyncJob::Sync;
        syncQueue_.push_front(next);
        runNextSync();
        return;
    }
    const Id world = res->ok ? res->report.world : job.world;
    if (res->ok) {
        syncFailed_[world] = false;
        syncChangesAt_[world] = lib_->changesMade();
        syncConflicts_[world] = res->report.conflicts;
        syncSkipped_[world] = res->report.skipped;
        const auto& r = res->report;
        std::wstring note;
        if (r.received + r.removedHere) note += plural((size_t)(r.received + r.removedHere), L"change", L"changes") + L" from GitHub";
        if (r.sent + r.removedThere) note += (note.empty() ? L"" : L", ") + plural((size_t)(r.sent + r.removedThere), L"change", L"changes") + L" sent";
        syncNote_[world] = note.empty() ? L"Up to date." : note + L".";
        if (job.world.empty()) {   // opened from GitHub
            openNote_.clear();
            setFieldText(FRepo, "");
            setFieldText(FToken, "");
            const World* wd = lib_->world(world);
            flash(L"“" + (wd ? w(wd->name) : std::wstring(L"The world")) + L"” is here from GitHub, and stays synced.", 6);
            goTo(View::World, world);
        } else if (!job.quiet || !r.conflicts.empty()) {
            flash(L"Synced with GitHub. " + syncNote_[world] + (r.conflicts.empty() ? L"" : L" Some things changed on both computers: see Sync."), 6);
        }
    } else {
        syncFailed_[world] = true;
        syncNote_[world] = w(res->error);
        if (job.world.empty()) {
            openNote_ = w(res->error);
            removeToken(job.repo);   // saved only for this open
        } else if (!job.quiet) {
            flash(L"Couldn't sync: " + w(res->error), 8);
        }
    }
    invalidate();
    runNextSync();
}

void StudioApp::syncAllSoon() {
    if (!lib_) return;
    for (const World* wd : lib_->liveWorlds()) {
        const sy::State s = sy::loadState(*lib_, wd->id);
        if (!s.repo.empty()) queueSync({SyncJob::Sync, wd->id, s.repo, lib_->folder(), true});
    }
}

void StudioApp::syncTick() {
    if (!lib_ || syncBusy_ || closing_) return;
    const ULONGLONG now = GetTickCount64();
    for (const World* wd : lib_->liveWorlds()) {
        const sy::State s = sy::loadState(*lib_, wd->id);
        if (s.repo.empty()) continue;
        auto tried = syncTriedAt_.find(wd->id);
        const ULONGLONG since = tried == syncTriedAt_.end() ? kCheckGap : now - tried->second;
        auto at = syncChangesAt_.find(wd->id);
        const bool changed = at == syncChangesAt_.end() || at->second != lib_->changesMade();
        if ((changed && since >= kQuietGap) || since >= kCheckGap) queueSync({SyncJob::Sync, wd->id, s.repo, lib_->folder(), true});
    }
}

// Closing: the last changes go to GitHub first (the window stays up, saying so, for as long as that takes).
void StudioApp::finishClosing() {
    closing_ = true;
    if (lib_) {
        for (const World* wd : lib_->liveWorlds()) {
            const sy::State s = sy::loadState(*lib_, wd->id);
            if (s.repo.empty() || loadToken(s.repo).empty()) continue;
            auto at = syncChangesAt_.find(wd->id);
            if (at == syncChangesAt_.end() || at->second != lib_->changesMade()) queueSync({SyncJob::Sync, wd->id, s.repo, lib_->folder(), true});
        }
        if (syncBusy_ || !syncQueue_.empty()) {
            flash(L"Sending your changes to GitHub before closing…", 120);
            invalidate();
            UpdateWindow(hwnd());
        }
        const ULONGLONG start = GetTickCount64();
        while (syncBusy_ || !syncQueue_.empty()) {
            if (GetTickCount64() - start > 120000) syncCancel_ = true;   // two minutes: stop, it'll go next time
            MSG m;
            while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
                if (m.message == WM_QUIT) { PostQuitMessage((int)m.wParam); break; }
                TranslateMessage(&m);
                DispatchMessageW(&m);
            }
            if (syncBusy_ || !syncQueue_.empty()) MsgWaitForMultipleObjects(0, nullptr, FALSE, 50, QS_ALLINPUT);
            if (!syncBusy_ && syncCancel_) syncQueue_.clear();
        }
    }
    if (syncThread_.joinable()) syncThread_.join();
    DestroyWindow(hwnd());
}

// ------------------------------------------------------------------------------------------- setting up

void StudioApp::startSyncSetup(const Id& world) {
    std::string owner, name;
    const std::string link = fieldText(FRepo);
    std::string token = fieldText(FToken);
    while (!token.empty() && std::isspace((unsigned char)token.back())) token.pop_back();
    while (!token.empty() && std::isspace((unsigned char)token.front())) token.erase(0, 1);
    auto say = [&](const std::wstring& m) {
        if (world.empty()) openNote_ = m;
        else { syncNote_[world] = m; syncFailed_[world] = true; }
        invalidate();
    };
    if (!sy::parseRepoLink(link, owner, name)) { say(L"That isn't a GitHub repository's link. It looks like https://github.com/you/your-world."); return; }
    if (!plausibleToken(token)) { say(L"Paste the whole token from GitHub (it starts github_pat_ or ghp_)."); return; }
    const std::string repo = owner + "/" + name;
    if (!world.empty())
        for (const World* wd : lib_->liveWorlds())
            if (wd->id != world && sy::loadState(*lib_, wd->id).repo == repo) { say(L"“" + w(wd->name) + L"” already syncs with that repository. Each world needs its own."); return; }
    if (!saveToken(repo, token)) { say(L"Windows wouldn't keep the token in Credential Manager."); return; }
    SecureZeroMemory(token.data(), token.size());
    setFieldText(FToken, "");
    if (world.empty()) openNote_ = L"Opening…";
    else syncNote_[world] = L"Checking the repository…";
    syncFailed_[world] = false;
    queueSync({world.empty() ? SyncJob::Sync : SyncJob::Check, world, repo, lib_->folder(), false});
    invalidate();
}

void StudioApp::stopSyncing(const Id& world) {
    const sy::State s = sy::loadState(*lib_, world);
    if (s.repo.empty()) return;
    const int a = cjui::messageBox(hwnd(),
                                   (L"Stop syncing this world with " + w(s.repo) +
                                    L"?\n\nNothing is deleted: the world stays here and the repository stays on GitHub. This computer forgets the "
                                    L"token.")
                                       .c_str(),
                                   L"Sync with GitHub", MB_YESNO | MB_ICONQUESTION);
    if (a != IDYES) return;
    sy::forget(*lib_, world);
    removeToken(s.repo);
    syncNote_.erase(world);
    syncConflicts_.erase(world);
    syncSkipped_.erase(world);
    syncFailed_.erase(world);
    flash(L"This world no longer syncs with GitHub.", 5);
    invalidate();
}

void StudioApp::replaceSyncToken(const Id& world) {
    const sy::State s = sy::loadState(*lib_, world);
    std::string token = fieldText(FToken);
    while (!token.empty() && std::isspace((unsigned char)token.back())) token.pop_back();
    while (!token.empty() && std::isspace((unsigned char)token.front())) token.erase(0, 1);
    if (!plausibleToken(token)) { syncNote_[world] = L"Paste the whole token from GitHub (it starts github_pat_ or ghp_)."; syncFailed_[world] = true; invalidate(); return; }
    if (!saveToken(s.repo, token)) { syncNote_[world] = L"Windows wouldn't keep the token in Credential Manager."; syncFailed_[world] = true; invalidate(); return; }
    SecureZeroMemory(token.data(), token.size());
    setFieldText(FToken, "");
    syncNote_[world] = L"Token saved.";
    syncFailed_[world] = false;
    queueSync({SyncJob::Sync, world, s.repo, lib_->folder(), false});
}

// ------------------------------------------------------------------------------------------- pages

void StudioApp::openSyncPage() {
    commitFields();
    openEntry({});
    wb_.syncPage = true;
    wb_.scroll = 0;
    setFocus({Focus::Sync, wb_.world, {}});
    invalidate();
}

float StudioApp::pageField(const Rect& r, float y, const std::wstring& label, Field f) {
    gfx_.text(label, {r.x, y, r.w, 18}, Font::Small, pal_.inkMuted, {Align::Left, false});
    y += 22;
    const Rect box{r.x, y, r.w, 34};
    gfx_.fillRounded(box, 3, pal_.surface);
    gfx_.strokeRounded(box, 3, GetFocus() == fields_[f] ? pal_.ribbon : pal_.rule);
    if (box.y >= pageRect_.y && box.bottom() <= pageRect_.bottom()) placeField(f, box);
    return y + 34 + 14;
}

void StudioApp::drawSync(const Rect& r) {
    gfx_.pushClip(r);
    setHitClip(r);
    const float pad = r.w > 900 ? 40 : 28;
    const float cw = std::min(r.w - pad * 2, 760.0f);
    const Rect m{r.x + pad, r.y + 26 - wb_.scroll, cw, 0};
    float y = m.y;
    const Id world = wb_.world;
    const sy::State s = sy::loadState(*lib_, world);
    gfx_.text(L"WORLDBUILDING", {m.x, y, cw, 16}, Font::Label, pal_.inkFaint, {Align::Left, false});
    y += 22;
    gfx_.text(L"Sync with GitHub", {m.x, y, cw, 40}, Font::SerifTitle, pal_.ink, {Align::Left, false});
    y += 48;
    y += gfx_.text(L"Keep this world in a private GitHub repository of your own, so you can write on more than one computer. Everything in "
                   L"the world goes: its series and books with their chapters, plans and comments, the bible, boards, languages, fonts and "
                   L"pictures (each picture stored once). Writing Studio sends and fetches changes by itself.",
                   {m.x, y, cw, 120}, Font::Body, pal_.inkMuted) + 22;
    auto note = [&](const std::wstring& t, bool bad) {
        const float h = gfx_.measureHeight(t, cw - 28, Font::Small);
        gfx_.fillRounded({m.x, y, cw, h + 20}, 4, bad ? cjui::hexColor("#C0392B").withAlpha(0.12f) : pal_.sunken);
        gfx_.text(t, {m.x + 14, y + 10, cw - 28, h + 4}, Font::Small, pal_.ink);
        y += h + 34;
    };
    const auto nt = syncNote_.find(world);
    const bool failed = syncFailed_.count(world) && syncFailed_[world];
    if (s.repo.empty()) {
        // Not set up yet: three steps.
        auto step = [&](int n, const std::wstring& title, const std::wstring& text) {
            gfx_.circle(m.x + 12, y + 11, 12, pal_.ribbon.withAlpha(0.16f), true);
            gfx_.text(std::to_wstring(n), {m.x, y, 24, 22}, Font::SmallStrong, pal_.ink, {Align::Center, false, false, true});
            gfx_.text(title, {m.x + 36, y + 1, cw - 36, 20}, Font::BodyStrong, pal_.ink, {Align::Left, false});
            y += 26;
            y += gfx_.text(text, {m.x + 36, y, cw - 36, 100}, Font::Small, pal_.inkMuted) + 10;
        };
        step(1, L"Make a private repository on GitHub",
             L"An empty one is best. Name it after the world, and choose Private, so only you can read it.");
        linkText("sync:new", m.x + 36, y, L"Open GitHub's new repository page", [] { cjui::openInBrowser("https://github.com/new"); });
        y += 40;
        step(2, L"Make a token for it",
             L"On GitHub: Settings, Developer settings, Fine-grained tokens, Generate new token. Under Repository access choose Only "
             L"select repositories and pick this one; under Permissions give Contents: Read and write. Copy the token it shows.");
        linkText("sync:token", m.x + 36, y, L"Open GitHub's token page",
                 [] { cjui::openInBrowser("https://github.com/settings/personal-access-tokens/new"); });
        y += 40;
        step(3, L"Put them here", L"The token is kept in Windows Credential Manager on this computer, never in the library.");
        const Rect fr{m.x + 36, y, std::min(cw - 36, 560.0f), 0};
        y = pageField(fr, y, L"The repository's link", FRepo);
        y = pageField(fr, y, L"The token", FToken);
        float bw = 0;
        smallButton("sync:start", fr.x, y, L"Start syncing", [this, world] { startSyncSetup(world); }, &bw);
        y += 50;
        if (nt != syncNote_.end() && !nt->second.empty()) note(nt->second, failed);
        y += gfx_.text(L"On another computer, open the same repository with Open a world from GitHub on the Library's Worlds page, using a "
                       L"token made there (or the same one).",
                       {m.x, y, cw, 80}, Font::Small, pal_.inkFaint) + 20;
    } else {
        const bool running = syncBusy_ && syncRunning_ == world;
        gfx_.text(L"REPOSITORY", {m.x, y, cw, 16}, Font::Label, pal_.inkFaint, {Align::Left, false});
        y += 22;
        linkText("sync:open", m.x, y, w(s.repo), [repo = s.repo] { cjui::openInBrowser("https://github.com/" + repo); });
        y += 40;
        gfx_.text(running ? L"Syncing now…" : L"Last synced " + agoText(s.lastSync) + L".", {m.x, y, cw, 22}, Font::Body, pal_.ink,
                  {Align::Left, false});
        y += 30;
        const bool hasToken = !loadToken(s.repo).empty();
        float bx = m.x, bw = 0;
        if (hasToken) {
            smallButton("sync:now", bx, y, running ? L"Syncing…" : L"Sync now", [this, world, repo = s.repo] {
                if (!syncBusy_ || syncRunning_ != world) queueSync({SyncJob::Sync, world, repo, lib_->folder(), false});
            }, &bw);
            bx += bw + 8;
        }
        smallButton("sync:stop", bx, y, L"Stop syncing…", [this, world] { stopSyncing(world); }, &bw);
        y += 52;
        if (nt != syncNote_.end() && !nt->second.empty()) note(nt->second, failed);
        if (!hasToken)
            note(L"This computer doesn't have the token for " + w(s.repo) + L". Paste one below to sync from here.", true);
        auto conf = syncConflicts_.find(world);
        if (conf != syncConflicts_.end() && !conf->second.empty()) {
            gfx_.text(L"CHANGED ON BOTH COMPUTERS", {m.x, y, cw, 16}, Font::Label, pal_.inkFaint, {Align::Left, false});
            y += 24;
            for (auto& c : conf->second) y += gfx_.text(L"•  " + w(c), {m.x, y, cw, 60}, Font::Small, pal_.ink) + 6;
            y += 14;
        }
        auto sk = syncSkipped_.find(world);
        if (sk != syncSkipped_.end() && !sk->second.empty()) {
            y += gfx_.text(plural(sk->second.size(), L"picture or font is", L"pictures or fonts are") +
                               L" over GitHub's 100 MB limit for a file, so they stay on this computer only.",
                           {m.x, y, cw, 60}, Font::Small, pal_.inkMuted) + 14;
        }
        gfx_.text(L"WHEN IT SYNCS", {m.x, y, cw, 16}, Font::Label, pal_.inkFaint, {Align::Left, false});
        y += 24;
        y += gfx_.text(L"When the library opens; every few minutes while you're changing things (and every ten minutes anyway, for changes from "
                       L"your other computers); and when Writing Studio closes. If the same chapter was changed on two computers before they "
                       L"synced, the newer text is kept and the other is a saved version in Review, Changes.",
                       {m.x, y, cw, 120}, Font::Small, pal_.inkMuted) + 24;
        gfx_.text(L"A NEW TOKEN", {m.x, y, cw, 16}, Font::Label, pal_.inkFaint, {Align::Left, false});
        y += 22;
        const Rect fr{m.x, y, std::min(cw, 560.0f), 0};
        y = pageField(fr, y, L"When the old one expires, or for this computer", FToken);
        smallButton("sync:replace", fr.x, y, L"Save the token", [this, world] { replaceSyncToken(world); }, &bw);
        y += 52;
    }
    wb_.content = y + wb_.scroll - r.y + 40;
    clearHitClip();
    gfx_.popClip();
}

void StudioApp::drawSyncPanel(const Rect& r) {
    gfx_.pushClip(r);
    setHitClip(r);
    const Rect c{r.x + 20, r.y, r.w - 40, 0};
    float y = r.y + 18 - wb_.panelScroll;
    panelHeading(c, y, L"SYNC", true);
    y += gfx_.text(L"Your own repository holds the world as small readable files: one for each chapter, entry, series and book, and each "
                   L"picture once under a name made from its contents. Change things in Writing Studio rather than on GitHub's site.",
                   {c.x, y, c.w, 200}, Font::Small, pal_.inkMuted) + 12;
    y += gfx_.text(L"No NorthRay account is involved: Writing Studio talks only to GitHub, with your token.", {c.x, y, c.w, 80}, Font::Small,
                   pal_.inkFaint) + 12;
    wb_.panelContent = y + wb_.panelScroll - r.y + 20;
    clearHitClip();
    gfx_.popClip();
}

float StudioApp::pageFromGitHub(const Rect& r) {
    float y = r.y;
    y += pageHeader(r, L"AUTHOR LIBRARY", L"Open a world from GitHub",
                    L"A world kept in GitHub from another computer (Worldbuilding, Sync with GitHub there) comes into this library and stays synced.");
    const Rect fr{r.x, y, std::min(r.w, 560.0f), 0};
    y = pageField(fr, y, L"The repository's link", FRepo);
    y = pageField(fr, y, L"A token for it (Contents: Read and write; kept in Windows Credential Manager)", FToken);
    float bw = 0;
    const bool opening = syncBusy_ && syncRunning_ == "-";
    smallButton("gh:open", r.x, y, opening ? L"Opening…" : L"Open the world", [this, opening] { if (!opening) startSyncSetup({}); }, &bw);
    linkText("gh:token", r.x + bw + 20, y + 6, L"Make a token on GitHub", [] { cjui::openInBrowser("https://github.com/settings/personal-access-tokens/new"); });
    y += 52;
    if (!openNote_.empty()) {
        const float h = gfx_.measureHeight(openNote_, std::min(r.w, 700.0f) - 28, Font::Small);
        gfx_.fillRounded({r.x, y, std::min(r.w, 700.0f), h + 20}, 3, pal_.sunken);
        gfx_.text(openNote_, {r.x + 14, y + 10, std::min(r.w, 700.0f) - 28, h + 4}, Font::Small, pal_.ink);
        y += h + 36;
    }
    return y - r.y;
}

}  // namespace nrs

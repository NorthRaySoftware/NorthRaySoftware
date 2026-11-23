// Keeping a world in a GitHub repository (0.61), so it can be written on more than one computer.
//
// A world is written out as small files (one per chapter, bible entry, series, book, setting...; see
// Library::worldFiles) plus its pictures and fonts, each picture kept once under the name of its own content (the
// library's objects store: the "NorthRay file store"). Syncing compares three states of every file: as it was at the
// last sync (kept in the library), as it is here, and as it is in the repository. A file changed on one side only
// goes to the other; changed on both, the newer wins and the other is kept (a chapter's other text as a saved
// version). The repository is reached through `Remote`; `GitHubRemote` talks to GitHub's REST API through an HTTP
// function the app supplies, so nothing here touches the network or the token's storage.
#pragma once

#include "studio/library.hpp"

#include <atomic>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace studio::sync {

// Git's name for some bytes: SHA-1 of "blob <size>\0" and them, in hex.
std::string gitBlobSha(const std::string& bytes);
std::string sha1Hex(const std::string& bytes);
std::string base64Encode(const std::string& bytes);
std::string base64Decode(const std::string& text);   // skips line breaks; "" for bad input

// A path a world file may have. Anything else from a repository is ignored.
bool validPath(const std::string& path);

// The repository's side.
struct RemoteState {
    std::string commit;                              // "" when the repository is empty
    std::string tree;
    std::map<std::string, std::string> files;        // path -> git blob sha
};
struct RepoInfo {
    bool isPrivate = false;
    std::string defaultBranch;
};

class Remote {
public:
    virtual ~Remote() = default;
    virtual bool info(RepoInfo& out, std::string& error) = 0;
    virtual bool head(RemoteState& out, std::string& error) = 0;
    virtual bool getBlob(const std::string& sha, std::string& bytes, std::string& error) = 0;
    // Makes a commit on top of `parent` with these changes (bytes, or nullopt to delete) and moves the branch to it.
    // `moved` is set when the branch has moved on since `parent` (another computer synced in between).
    virtual bool commit(const RemoteState& parent, const std::map<std::string, std::optional<std::string>>& changes,
                        const std::string& message, std::string& newCommit, bool& moved, std::string& error) = 0;
};

// A repository held in memory, for tests.
class MemoryRemote : public Remote {
public:
    bool info(RepoInfo& out, std::string& error) override;
    bool head(RemoteState& out, std::string& error) override;
    bool getBlob(const std::string& sha, std::string& bytes, std::string& error) override;
    bool commit(const RemoteState& parent, const std::map<std::string, std::optional<std::string>>& changes,
                const std::string& message, std::string& newCommit, bool& moved, std::string& error) override;
    std::map<std::string, std::string> files;        // path -> bytes
    std::string commitId;
    int commits = 0;
    bool isPrivate = true;
};

// GitHub, through its REST API (git data endpoints). `http` does one request: method, full URL, headers, body;
// it returns the status (0 when it couldn't connect) and the body. The token is only ever put in the
// Authorization header and never in an error message.
struct HttpResponse {
    int status = 0;
    std::string body;
    std::string error;
};
using Http = std::function<HttpResponse(const std::string& method, const std::string& url,
                                        const std::vector<std::pair<std::string, std::string>>& headers, const std::string& body)>;

class GitHubRemote : public Remote {
public:
    GitHubRemote(std::string owner, std::string repo, std::string token, Http http, std::string branch = {});
    bool info(RepoInfo& out, std::string& error) override;
    bool head(RemoteState& out, std::string& error) override;
    bool getBlob(const std::string& sha, std::string& bytes, std::string& error) override;
    bool commit(const RemoteState& parent, const std::map<std::string, std::optional<std::string>>& changes,
                const std::string& message, std::string& newCommit, bool& moved, std::string& error) override;
    const std::string& branch() const { return branch_; }
    std::string apiBase = "https://api.github.com";
    // Called as files are sent (how many so far, of how many), from the thread doing the sending.
    std::function<void(size_t done, size_t total)> onProgress;

private:
    HttpResponse call(const std::string& method, const std::string& path, const std::string& body = {});
    std::string describe(const HttpResponse& r, const std::string& what) const;
    bool ensureBranch(std::string& error);
    std::string owner_, repo_, token_, branch_;
    Http http_;
};

// "https://github.com/owner/repo", "github.com/owner/repo.git", "git@github.com:owner/repo.git" or "owner/repo".
bool parseRepoLink(const std::string& link, std::string& owner, std::string& repo);

// What the library keeps about a world's repository (setting "sync:world:<id>"; never synced itself).
struct State {
    std::string repo;                                // "owner/name"
    std::string branch;
    std::string commit;                              // the repository's commit at the last sync
    std::map<std::string, std::string> files;        // path -> git blob sha at the last sync
    std::int64_t lastSync = 0;
};
State loadState(const Library& lib, const Id& world);
bool saveState(Library& lib, const Id& world, const State& s);
bool forget(Library& lib, const Id& world);          // stop syncing (nothing is deleted anywhere)

struct Report {
    Id world;                                        // the world synced (found in the repository when opening one)
    int received = 0, sent = 0, removedHere = 0, removedThere = 0;
    std::vector<std::string> conflicts;              // what changed on both sides, said plainly
    std::vector<std::string> skipped;                // pictures too big for GitHub and the like
    bool changedHere() const { return received + removedHere > 0; }
};

// This computer's side, so the app can run the network part away from its window's thread while the library is
// only touched on it. `LibraryLocal` does each step straight on a library.
class Local {
public:
    virtual ~Local() = default;
    virtual std::map<std::string, Library::WorldFile> files(const Id& world) = 0;
    virtual bool hasWorld(const Id& world) = 0;
    virtual std::string worldName(const Id& world) = 0;
    virtual bool apply(const Id& world, const std::map<std::string, std::optional<std::string>>& changes,
                       const std::vector<std::pair<Id, std::string>>& keepVersions, std::string& error) = 0;
    virtual void save(const Id& world, const State& state) = 0;
};
class LibraryLocal : public Local {
public:
    explicit LibraryLocal(Library& lib) : lib_(lib) {}
    std::map<std::string, Library::WorldFile> files(const Id& world) override { return lib_.worldFiles(world); }
    bool hasWorld(const Id& world) override { return lib_.world(world) != nullptr; }
    std::string worldName(const Id& world) override { const World* w = lib_.world(world); return w ? w->name : std::string(); }
    bool apply(const Id& world, const std::map<std::string, std::optional<std::string>>& changes,
               const std::vector<std::pair<Id, std::string>>& keepVersions, std::string& error) override {
        return lib_.applyWorldFiles(world, changes, keepVersions, error);
    }
    void save(const Id& world, const State& state) override { saveState(lib_, world, state); }

private:
    Library& lib_;
};

// One sync of `world` with `remote`. With an empty `world`, the world is opened from the repository (it must not be
// in this library already). The library is read again when anything here changed. `cancel`, when set, stops it
// between files (nothing half-done: the repository and the library each change in one step).
bool syncWorld(Local& local, const Id& world, Remote& remote, State& state, Report& report, std::string& error,
               const std::atomic<bool>* cancel = nullptr);
bool syncWorld(Library& lib, const Id& world, Remote& remote, State& state, Report& report, std::string& error);

constexpr std::size_t kMaxFileBytes = 95u * 1024 * 1024;   // GitHub refuses files over 100 MB

}  // namespace studio::sync

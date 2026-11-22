// Syncs a library's world into a fresh library through an in-memory repository, and says what went wrong if it
// does (for checking sync against real libraries): studio_sync_dump <library> <empty folder>
#include "studio/library.hpp"
#include "studio/sync.hpp"

#include <cstdio>

int main(int argc, char** argv) {
    if (argc < 3) { std::fprintf(stderr, "usage: studio_sync_dump <library> <empty folder>\n"); return 2; }
    studio::OpenResult r1, r2;
    auto a = studio::Library::open(argv[1], r1);
    auto b = studio::Library::open(argv[2], r2);
    if (!a || !b) { std::fprintf(stderr, "open failed\n"); return 1; }
    int bad = 0;
    for (const studio::World* w : a->liveWorlds()) {
        studio::sync::MemoryRemote repo;
        studio::sync::State sa, sb;
        studio::sync::Report rep;
        std::string err;
        const bool up = studio::sync::syncWorld(*a, w->id, repo, sa, rep, err);
        std::printf("%s: sent %d files %s\n", w->name.c_str(), rep.sent, up ? "" : err.c_str());
        const bool down = studio::sync::syncWorld(*b, {}, repo, sb, rep, err);
        std::printf("  opened: %s received %d %s\n", down ? "yes" : "no", rep.received, down ? "" : err.c_str());
        if (!up || !down) ++bad;
        // What differs after the round trip.
        if (down) {
            const auto fa = a->worldFiles(w->id), fb = b->worldFiles(w->id);
            for (auto& [p, f] : fa) if (!fb.count(p) || (f.diskPath.empty() && fb.at(p).bytes != f.bytes)) { std::printf("  differs: %s\n", p.c_str()); ++bad; }
            // A change made on the second library comes back to the first.
            studio::World nw = *b->world(w->id);
            nw.description += " (changed on the second)";
            b->updateWorld(nw);
            const bool up2 = studio::sync::syncWorld(*b, w->id, repo, sb, rep, err);
            const bool back = studio::sync::syncWorld(*a, w->id, repo, sa, rep, err);
            const bool got = a->world(w->id)->description == nw.description;
            std::printf("  back again: %s %s\n", up2 && back && got ? "yes" : "no", err.c_str());
            if (!(up2 && back && got)) ++bad;
        }
    }
    return bad ? 1 : 0;
}

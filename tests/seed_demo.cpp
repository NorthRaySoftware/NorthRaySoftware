// Developer tool: makes a small demo library (an invented world and book) for
// screenshots of NorthRay Writing Studio. Usage: studio_seed_demo <folder>
#include "studio/document.hpp"
#include "studio/library.hpp"

#include <cstdio>
#include <nlohmann/json.hpp>

using namespace studio;

static std::string paras(std::initializer_list<const char*> ps) {
    doc::Builder b;
    for (const char* p : ps) { b.paragraph(); b.text(p); }
    return b.toJson();
}

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: studio_seed_demo <folder>\n"); return 2; }
    OpenResult r;
    auto lib = Library::open(argv[1], r);
    if (!lib) { std::fprintf(stderr, "open failed: %s\n", r.error.c_str()); return 1; }

    const Id world = lib->createWorld("The Lantern Coast");
    if (World w = *lib->world(world); true) {
        w.description = "A stormy coastline of lighthouses, salt towns and the people who keep the lights burning.";
        w.accent = "#2E7D8C";
        lib->updateWorld(w);
    }
    const Id series = lib->createSeries("Keepers of the Light", world);
    const Id book = lib->createBook("The Last Light at Harrow Point", world, series);
    if (Book b = *lib->book(book); true) {
        b.subtitle = "Keepers of the Light, Book One";
        b.author = "Demo Author";
        b.genre = "Fantasy";
        b.synopsis = "When the great lamp at Harrow Point goes dark, a lighthouse keeper's apprentice has seven nights to relight it.";
        b.status = BookStatus::Drafting;
        lib->updateBook(b);
    }
    const Id book2 = lib->createBook("The Drowned Bell", world, series);
    if (Book b = *lib->book(book2); true) { b.status = BookStatus::Planning; b.author = "Demo Author"; lib->updateBook(b); }
    const Id other = lib->createBook("Notes from a Quiet Garden", "");
    if (Book b = *lib->book(other); true) { b.status = BookStatus::Idea; b.author = "Demo Author"; lib->updateBook(b); }

    // Worldbuilding.
    auto entry = [&](const char* kind, const char* name, const char* summary, const char* details) {
        const Id id = lib->createEntry(world, "", kind, name);
        Entry e = *lib->entry(id);
        e.summary = summary;
        e.details = details;
        lib->updateEntry(e);
        return id;
    };
    const Id isla = entry("character", "Isla Merrow", "Apprentice keeper at Harrow Point. Stubborn, curious, afraid of deep water.",
                          "Age: 17\nRole: Apprentice keeper\nHome: Harrow Point");
    const Id tomas = entry("character", "Tomas Reed", "The old keeper. Knows more about the lamp than he admits.",
                           "Age: 68\nRole: Head keeper");
    const Id wren = entry("character", "Wren Calloway", "A ferry pilot who trades news for supper.", "Role: Ferry pilot");
    const Id harrow = entry("place", "Harrow Point", "A lighthouse on a granite spur at the edge of the coast.", "Region: The Lantern Coast");
    entry("place", "Saltmarket", "The nearest town: fish, rope and rumours.", "");
    const Id guild = entry("group", "The Tide Guild", "The keepers' guild, older than any lighthouse on the coast.", "Founded: Before the first lamp");
    entry("thing", "The Great Lamp", "The lamp at Harrow Point. Nobody remembers who made it.", "");
    lib->addLink(isla, tomas, "apprentice of");
    lib->addLink(isla, harrow, "lives at");
    lib->addLink(tomas, guild, "member of");
    lib->addLink(wren, isla, "friend of");

    // The manuscript.
    const Id p1 = lib->createNode(book, {}, NodeKind::Part, "Part One: The Dark Night");
    struct Ch { const char* title; const char* synopsis; NodeStatus st; Id pov; long target; std::string text; };
    const Ch chapters[] = {
        {"The Lamp Goes Out", "Isla is alone on watch when the great lamp fails.", NodeStatus::Revised, isla, 3000,
         paras({"The lamp at Harrow Point had burned for three hundred years, and on the night it went out Isla Merrow was the only one awake to see it.",
                "She had been counting ships. It was a game Tomas had taught her in her first winter: a lantern for every hull that passed safely, a pebble for every one that didn't. The jar of pebbles on the windowsill had been empty for as long as she could remember.",
                "Then the light above her shuddered, thinned to a thread, and was gone.",
                "For a long moment there was nothing but the wind and the sea, and Isla's own heartbeat loud in her ears."})},
        {"Tomas Knows", "The old keeper reacts too calmly. Isla suspects he has seen this before.", NodeStatus::Draft, isla, 3000,
         paras({"Tomas didn't run. That was the first thing Isla noticed. He climbed the hundred and twelve steps at his usual pace, one hand on the rail, and when he reached the lamp room he simply looked at the cold glass for a while.",
                "\"Seven nights,\" he said at last. \"We have seven nights before the Guild hears of it.\"",
                "\"And then what?\"",
                "He didn't answer, and Isla found that she didn't want him to."})},
        {"The Ferry to Saltmarket", "Wren brings news: other lights along the coast are failing too.", NodeStatus::Draft, wren, 2500,
         paras({"Wren Calloway's ferry came in on the morning tide, low in the water and smelling of tar.",
                "\"You've gone dark,\" she called up from the jetty, as if Isla might not have noticed. \"So has Gullstone. So has the Needle.\""})},
        {"What the Guild Remembers", "", NodeStatus::Outline, tomas, 3500, ""},
    };
    for (const Ch& c : chapters) {
        const Id id = lib->createNode(book, p1, NodeKind::Chapter, c.title);
        ManuscriptNode n = *lib->node(id);
        n.synopsis = c.synopsis;
        n.status = c.st;
        n.povId = c.pov;
        n.targetWords = c.target;
        lib->updateNode(n);
        if (!c.text.empty()) lib->saveNodeContent(id, c.text);
    }
    const Id p2 = lib->createNode(book, {}, NodeKind::Part, "Part Two: Seven Nights");
    for (const char* t : {"The First Night", "Under the Granite", "The Keeper's Journal"}) {
        const Id id = lib->createNode(book, p2, NodeKind::Chapter, t);
        ManuscriptNode n = *lib->node(id);
        n.status = NodeStatus::Outline;
        n.targetWords = 3000;
        lib->updateNode(n);
    }
    lib->setSetting("plan:" + book, R"({"targetWords":80000})");
    lib->setSetting("goal:daily", "500");
    lib->takeSnapshot(book, "First draft of Part One");
    // A revision since the snapshot, so Review has changes to show.
    const Id first = lib->manuscriptOf(book)[1]->id;
    lib->saveNodeContent(first, paras({
        "The lamp at Harrow Point had burned for three hundred years, and on the night it went out Isla Merrow was the only one awake to see it.",
        "She had been counting ships, the way Tomas had taught her in her first winter: a lantern for every hull that passed safely, a pebble for every one that didn't. The jar of pebbles on the windowsill had been empty for as long as she could remember.",
        "Then the light above her shuddered, thinned to a silver thread, and went out.",
        "For a long moment there was nothing but the wind and the sea, and Isla's own heartbeat loud in her ears.",
        "Somewhere below, a door opened."}));
    // Comments: a note on some words, as the editor makes them.
    auto commentOn = [&](const Id& nodeId, const std::string& phrase, const std::string& id, const std::string& body, bool resolved) {
        nlohmann::json d = nlohmann::json::parse(lib->nodeContent(nodeId));
        for (auto& para : d["content"]) {
            if (!para.contains("content")) continue;
            nlohmann::json out = nlohmann::json::array();
            bool done = false;
            for (auto& t : para["content"]) {
                const std::string text = t.value("text", "");
                const size_t at = done ? std::string::npos : text.find(phrase);
                if (at == std::string::npos) { out.push_back(t); continue; }
                done = true;
                auto piece = [&](const std::string& x, bool mark) {
                    if (x.empty()) return;
                    nlohmann::json n = t;
                    n["text"] = x;
                    if (mark) n["marks"].push_back({{"type", "comment"}, {"attrs", {{"id", id}}}});
                    out.push_back(n);
                };
                piece(text.substr(0, at), false);
                piece(phrase, true);
                piece(text.substr(at + phrase.size()), false);
            }
            para["content"] = out;
            if (done) break;
        }
        lib->saveNodeContent(nodeId, d.dump());
        lib->addComment(nodeId, id, phrase);
        lib->updateComment(id, body);
        if (resolved) lib->resolveComment(id, true, false);
    };
    const auto ms = lib->manuscriptOf(book);
    commentOn(ms[1]->id, "three hundred years", "a1b2c3d4e5f60718", "Check this against the lighthouse's history in the bible.", false);
    commentOn(ms[1]->id, "Somewhere below, a door opened.", "b2c3d4e5f6071829", "Strong ending. Maybe hold it for the next chapter?", false);
    commentOn(ms[2]->id, "Seven nights", "c3d4e5f60718293a", "Make sure the count of nights stays the same in Part Two.", false);
    commentOn(ms[3]->id, "smelling of tar", "d4e5f60718293a4b", "Too close to the opening of chapter one?", true);
    lib->ensureEditions(book);
    std::printf("demo library made in %s\n", argv[1]);
    return 0;
}

// Typesets a sample book and writes page images (PPM) and a PDF, for looking at
// the typesetter's output. Usage: studio_typeset_dump <out folder> [preset] [picture.png]
#include "studio/ebook.hpp"
#include "studio/import.hpp"
#include "studio/library.hpp"
#include "studio/publish.hpp"
#include "studio/typeset.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;
using namespace studio;

static std::string fixture(const char* name) {
    for (const char* base : {"tests/fixtures/", "../tests/fixtures/", "../../tests/fixtures/"})
        if (fs::exists(std::string(base) + name)) return std::string(base) + name;
    return std::string("tests/fixtures/") + name;
}
static std::string appData(const char* sub) {
    for (const char* base : {"apps/studio/", "../apps/studio/", "../../apps/studio/"})
        if (fs::exists(std::string(base) + sub)) return std::string(base) + sub;
    return std::string("apps/studio/") + sub;
}

static std::string nlohmannQuote(const std::string& s) {
    std::string o = "\"";
    for (unsigned char c : s) {
        if (c == '"') o += "\\\"";
        else if (c == '\\') o += "\\\\";
        else if (c == '\n') o += "\\n";
        else if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); o += b; }
        else o += (char)c;
    }
    return o + "\"";
}

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: studio_typeset_dump <out> [preset] [picture]\n"); return 2; }
    const fs::path out = argv[1];
    fs::create_directories(out);
    const std::string presetId = argc > 2 ? argv[2] : "classic";
    const fs::path libDir = out / "library";
    fs::remove_all(libDir);
    OpenResult r;
    auto lib = Library::open(libDir.string(), r);
    if (!lib) { std::fprintf(stderr, "%s\n", r.error.c_str()); return 1; }
    const Id book = lib->createBook("The Glass Harbour", {});
    Book b = *lib->book(book);
    b.subtitle = "A Novel";
    b.author = "Ada Example";
    lib->updateBook(b);
    auto plan = import::readFile(fixture("novel.epub"));
    import::apply(*lib, book, plan);
    // A longer chapter of original sample prose, with italics, small capitals, a scene break and a picture.
    std::string para = R"({"type":"paragraph","content":[{"type":"text","text":"The harbour master kept a ledger of every vessel that had ever sheltered behind the breakwater, and on winter evenings, when the lamps were trimmed and the tide was running out across the flats, he would read the entries aloud to anyone who cared to listen. Most of the names meant nothing to the children who gathered by the stove: schooners and trawlers, a lifeboat, once a "},{"type":"text","marks":[{"type":"em"}],"text":"pleasure yacht"},{"type":"text","text":" that had limped in with a broken mast and an extraordinarily disagreeable owner. But each entry carried a small story, and the stories accumulated, season after season, into something that felt very like a history of the town itself."}]})";
    std::string para2 = R"({"type":"paragraph","content":[{"type":"text","text":"Nobody remembered who had started the custom. "},{"type":"text","marks":[{"type":"smallcaps"}],"text":"Captain Harlow"},{"type":"text","text":" claimed it had been his grandmother; the schoolteacher insisted that the practice was older than the harbour, older than the quay, and had been carried across from the islands by families who had always measured their lives by the comings and goings of boats. Either way, it was understood that the ledger was not to be hurried."}]})";
    std::string pic;
    if (argc > 3) {
        std::ifstream in(argv[3], std::ios::binary);
        std::stringstream ss;
        ss << in.rdbuf();
        const Id art = lib->addArtwork(ss.str(), "Harbour chart", argv[3]);
        if (!art.empty()) pic = R"(,{"type":"image","attrs":{"blob":")" + lib->artwork(art)->blob + R"(","caption":"The harbour at low water","width":0.7,"cropX":0.1,"cropW":0.8}})";
    }
    std::string doc = R"({"type":"doc","content":[)" + para + "," + para2 + "," + para + R"(,{"type":"scene_break"},)" + para2 + pic;
    for (int i = 0; i < 6; ++i) doc += "," + (i % 2 ? para : para2);
    doc += "]}";
    const Id ch = lib->createNode(book, {}, NodeKind::Chapter, "The Ledger");
    lib->saveNodeContent(ch, doc);

    typeset::Design d = typeset::preset(presetId).design;
    typeset::Resources res{appData("fonts"), appData("hyphenation")};
    auto tb = typeset::typesetBook(*lib, book, d, res);
    if (!tb.ok) { std::fprintf(stderr, "typeset failed: %s\n", tb.error.c_str()); return 1; }
    std::printf("%zu pages, %zu fonts, %lld words\n", tb.pages.size(), tb.fonts.size(), (long long)tb.words);
    for (auto& w : tb.warnings) std::printf("warning: %s\n", w.c_str());
    for (size_t i = 0; i < tb.pages.size(); ++i) {
        auto bm = typeset::renderPage(tb, (int)i, 110);
        std::ofstream f(out / ("page-" + std::to_string(i + 1) + ".ppm"), std::ios::binary);
        f << "P6\n" << bm.width << " " << bm.height << "\n255\n";
        for (size_t p = 0; p < bm.rgba.size(); p += 4) f.write(reinterpret_cast<const char*>(&bm.rgba[p]), 3);
    }
    std::string err;
    if (!typeset::writePdf(tb, (out / "book.pdf").string(), err)) { std::fprintf(stderr, "pdf: %s\n", err.c_str()); return 1; }
    {
        typeset::PdfOptions po;
        po.printReady = true;
        po.bleed = true;
        if (!typeset::writePdf(tb, (out / "print.pdf").string(), err, po)) { std::fprintf(stderr, "print pdf: %s\n", err.c_str()); return 1; }
        const auto src = typeset::snapshot(*lib, book);
        const auto e = publish::writeEpub(src, (out / "book.epub").string());
        if (!e.ok) { std::fprintf(stderr, "epub: %s\n", e.error.c_str()); return 1; }
        for (const auto& c : publish::preflight(src, &tb))
            std::printf("[%s] %s: %s %s\n", c.level == publish::Check::Ok ? "ok" : c.level == publish::Check::Advice ? "advice" : "PROBLEM",
                        c.area.c_str(), c.title.c_str(), c.detail.c_str());
    }
    std::printf("wrote %s\n", (out / "book.pdf").string().c_str());
    // The ebook preview's content, for the browser check.
    {
        auto content = ebook::build(typeset::snapshot(*lib, book), [](const std::string& h) {
            return "https://objects.nrstudio.localhost/" + h.substr(0, 2) + "/" + h.substr(2);
        });
        std::string j = "{\"css\":" + nlohmannQuote(ebook::stylesheet()) + ",\"chapters\":[";
        for (size_t i = 0; i < content.chapters.size(); ++i)
            j += std::string(i ? "," : "") + "{\"title\":" + nlohmannQuote(content.chapters[i].title) + ",\"part\":" +
                 (content.chapters[i].part ? "true" : "false") + ",\"html\":" + nlohmannQuote(content.chapters[i].html) + "}";
        j += "],\"objects\":" + nlohmannQuote(libDir.string() + "/objects") + "}";
        std::ofstream(out / "ebook.json") << j;
    }
    return 0;
}

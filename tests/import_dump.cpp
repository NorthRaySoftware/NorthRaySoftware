// Prints what the importer finds in a file: studio_import_dump <file>
#include "studio/document.hpp"
#include "studio/import.hpp"

#include <cstdio>

int main(int argc, char** argv) {
    if (argc < 2) { std::puts("usage: studio_import_dump <file> [--text]"); return 2; }
    const bool text = argc > 2 && std::string(argv[2]) == "--text";
    auto plan = studio::import::readFile(argv[1]);
    std::printf("format: %s  ok: %d  approximate: %d\n", studio::import::label(plan.format), plan.ok, plan.approximate);
    if (!plan.ok) { std::printf("error: %s\n", plan.error.c_str()); return 1; }
    std::printf("title: %s | subtitle: %s | author: %s | lang: %s | isbn: %s\n", plan.meta.title.c_str(), plan.meta.subtitle.c_str(),
                plan.meta.author.c_str(), plan.meta.language.c_str(), plan.meta.isbn.c_str());
    std::printf("description: %s\ncover: %zu bytes %s | images: %zu\n", plan.meta.description.c_str(), plan.meta.coverBytes.size(),
                plan.meta.coverType.c_str(), plan.images.size());
    for (auto& s : plan.sections) {
        std::printf("%s[%c] %-18s %-40s %6lld words  (%s)\n", s.level ? "    " : "", s.include ? 'x' : ' ', studio::import::label(s.role),
                    s.title.c_str(), (long long)s.words, s.source.c_str());
        if (text) std::printf("%s\n---\n%s\n---\n", studio::doc::plainText(s.content).c_str(), s.content.c_str());
    }
    for (auto& w : plan.warnings) std::printf("warning: %s\n", w.c_str());
    std::printf("included: %d sections, %lld words\n", plan.includedCount(), (long long)plan.includedWords());
}

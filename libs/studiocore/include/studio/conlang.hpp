// Writing Studio: constructed languages (conlangs) in a world's bible. Portable and tested.
//
// A world (or a book on its own) can have languages, each a list of words with their meanings. Their words are
// words to the spell checker: a word from one of the languages isn't marked as a misspelling, and a near miss of one
// is marked with the language's word as the suggestion. Words can be affixes: "-ar" (an ending) or "ve-" (a
// beginning); a word made of a known word with known affixes counts as spelt right.
//
// Kept as JSON in the library's settings ("conlangs:world:<id>" or "conlangs:book:<id>"). Import reads:
//   - NorthRay's own file (.nrlang, JSON: one language, or several under "languages"),
//   - spreadsheets saved as CSV or TSV (comma, semicolon or tab; a header row names the columns: word / conword /
//     lemma / headword, meaning / gloss / definition / english / translation, part of speech / pos / class / type,
//     pronunciation / ipa, notes; without one the columns are word, meaning, part of speech),
//   - SFM / MDF lexicons (Toolbox, Lexique Pro, FieldWorks exports: \lx, \ge or \de, \ps, \ph, \nt),
//   - plain word lists (one a line, optionally "word = meaning", "word - meaning", "word: meaning" or "word<tab>meaning").
// Export writes .nrlang or CSV (UTF-8 with a header row, which spreadsheets and most conlang tools read).
#pragma once

#include <string>
#include <vector>

namespace studio::conlang {

struct Word {
    std::string word, meaning, part, pronunciation, notes;
    bool operator==(const Word&) const = default;
};

struct Language {
    std::string id;          // stable within the bible ("lang1"...)
    std::string name;        // "Telvani"
    std::string notes;
    std::vector<Word> words;
    bool operator==(const Language&) const = default;
};

constexpr std::size_t kMaxLanguages = 50;
constexpr std::size_t kMaxWords = 50000;   // per language
constexpr std::size_t kMaxFileBytes = 32u * 1024 * 1024;

// The bible's languages, as kept in settings. Bad entries are skipped; words are trimmed and empty ones dropped.
std::string toJson(const std::vector<Language>& langs);
std::vector<Language> fromJson(const std::string& json);

// A new id not used by `langs`.
std::string newId(const std::vector<Language>& langs);

// Reads a file's bytes as one or more languages. `fileName` picks the format by its extension (.nrlang/.json,
// .csv, .tsv, .txt, .sfm/.db/.mdf/.lex); text files are sniffed (SFM markers, tabs or delimiters). A language with
// no name in the file is named after the file. False with `error` in words if nothing could be read.
bool importFile(const std::string& fileName, const std::string& bytes, std::vector<Language>& out, std::string& error);

std::string exportJson(const Language& lang);   // .nrlang
std::string exportCsv(const Language& lang);    // UTF-8 with a BOM (so Excel reads the accents), CRLF lines

// Adds `incoming` to `lang`: new words are added, words already there (same spelling, ignoring case) get any
// meaning, part, pronunciation or notes they lacked. Returns how many were added.
std::size_t merge(Language& lang, const Language& incoming);

// What the spell checker needs from a language: its words (hyphenated and multi-word entries also split into their parts),
// and its affixes without their hyphens.
struct SpellData {
    std::vector<std::string> words, prefixes, suffixes;
};
SpellData spellData(const Language& lang);

}  // namespace studio::conlang

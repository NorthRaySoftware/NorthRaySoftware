// Internal helpers shared by the importers.
#pragma once

#include "studio/import.hpp"

#include "markup.hpp"
#include "miniz.h"

#include <cctype>
#include <map>

#include <string>
#include <vector>

namespace studio::import::detail {

inline std::string lowerAscii(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

// ------------------------------------------------------------------- ZIP

class Zip {
public:
    ~Zip() { if (open_) mz_zip_reader_end(&z_); }

    bool open(const std::string& bytes, const Limits& lim, std::string& err) {
        lim_ = lim;
        mz_zip_zero_struct(&z_);
        if (!mz_zip_reader_init_mem(&z_, bytes.data(), bytes.size(), 0)) { err = "The file isn't a readable ZIP archive (an EPUB is one)."; return false; }
        open_ = true;
        const mz_uint n = mz_zip_reader_get_num_files(&z_);
        if ((int)n > lim.maxEntries) { err = "The book contains far too many files to be a normal EPUB."; return false; }
        for (mz_uint i = 0; i < n; ++i) {
            mz_zip_archive_file_stat st;
            if (!mz_zip_reader_file_stat(&z_, i, &st)) continue;
            if (st.m_is_directory) continue;
            names_[st.m_filename] = i;
            lowerNames_[lowerAscii(st.m_filename)] = i;
        }
        return true;
    }
    bool has(const std::string& name) const { return find(name) >= 0; }
    std::vector<std::string> names() const {
        std::vector<std::string> v;
        for (auto& [k, i] : names_) v.push_back(k);
        return v;
    }
    // Reads one entry, enforcing the limits. Returns false with a reason.
    bool read(const std::string& name, std::string& out, std::string& err) {
        const int idx = find(name);
        if (idx < 0) { err = "missing " + name; return false; }
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&z_, (mz_uint)idx, &st)) { err = "unreadable " + name; return false; }
        if (st.m_is_encrypted) { err = "encrypted"; encryptedSeen_ = true; return false; }
        if (st.m_uncomp_size > lim_.maxEntryBytes) { err = name + " is too large"; return false; }
        if (st.m_uncomp_size > (1u << 20) && st.m_comp_size > 0 && st.m_uncomp_size / st.m_comp_size > (mz_uint64)lim_.maxRatio) {
            err = name + " is suspiciously compressed";
            return false;
        }
        if (total_ + st.m_uncomp_size > lim_.maxTotalBytes) { err = "the book unpacks to more than the size limit"; return false; }
        size_t size = 0;
        void* p = mz_zip_reader_extract_to_heap(&z_, (mz_uint)idx, &size, 0);
        if (!p) { err = "couldn't unpack " + name; return false; }
        out.assign(static_cast<const char*>(p), size);
        mz_free(p);
        total_ += size;
        return true;
    }
    bool encryptedSeen() const { return encryptedSeen_; }

private:
    int find(const std::string& name) const {
        auto it = names_.find(name);
        if (it != names_.end()) return (int)it->second;
        auto jt = lowerNames_.find(lowerAscii(name));  // some books get the case of links wrong
        return jt == lowerNames_.end() ? -1 : (int)jt->second;
    }
    mz_zip_archive z_;
    bool open_ = false;
    Limits lim_;
    std::map<std::string, mz_uint> names_, lowerNames_;
    std::uint64_t total_ = 0;
    bool encryptedSeen_ = false;
};


// Builds sections from (converted) HTML: splits at h1 (or h2) when there are
// several, otherwise finds chapters from the text itself ("Chapter 1", "***").
void sectionsFromHtml(Plan& plan, const std::string& html, const markup::HtmlContext& ctx, const std::string& fallbackTitle);

Role roleFrom(const std::string& epubType, const std::string& title, const std::string& file, const std::string& bodyClass);
std::string stripRepeatedTitle(const std::string& docJson, const std::string& title);
// Lines -> sections (headings, parts, scene breaks, paragraphs). Used by text,
// Markdown and PDF import. `fallbackTitle` names the book when it has no headings.
// 0: not a heading; 1: chapter ("Chapter 3", "IV"); 2: part ("Part One"); 3: prologue, epilogue and the like.
int headingKind(const std::string& line);
void buildFromLines(const std::vector<Line>& lines, Plan& plan, bool markdown, const std::string& fallbackTitle);

}  // namespace studio::import::detail

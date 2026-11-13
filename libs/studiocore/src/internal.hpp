// Internal helpers shared by the studiocore sources. Not a public header.
#pragma once

#include "sqlite3.h"

#include <cstdint>
#include <filesystem>
#include <string>

namespace studio::detail {

struct Stmt {
    sqlite3_stmt* s = nullptr;
    Stmt(sqlite3* db, const char* sql) { sqlite3_prepare_v2(db, sql, -1, &s, nullptr); }
    ~Stmt() { sqlite3_finalize(s); }
    explicit operator bool() const { return s != nullptr; }
    Stmt& bind(int i, const std::string& v) { sqlite3_bind_text(s, i, v.c_str(), (int)v.size(), SQLITE_TRANSIENT); return *this; }
    Stmt& bindIdOrNull(int i, const std::string& v) {
        if (v.empty()) sqlite3_bind_null(s, i); else bind(i, v);
        return *this;
    }
    Stmt& bind(int i, std::int64_t v) { sqlite3_bind_int64(s, i, v); return *this; }
    Stmt& bind(int i, double v) { sqlite3_bind_double(s, i, v); return *this; }
    bool run() { return sqlite3_step(s) == SQLITE_DONE; }
    bool row() { return sqlite3_step(s) == SQLITE_ROW; }
    std::string text(int c) const {
        const unsigned char* t = sqlite3_column_text(s, c);
        return t ? std::string(reinterpret_cast<const char*>(t), (size_t)sqlite3_column_bytes(s, c)) : std::string{};
    }
    std::int64_t i64(int c) const { return sqlite3_column_int64(s, c); }
    double dbl(int c) const { return sqlite3_column_double(s, c); }
};


inline std::filesystem::path u8path(const std::string& s) { return std::filesystem::path(std::u8string(s.begin(), s.end())); }
inline std::string fromPath(const std::filesystem::path& p) {
    const std::u8string s = p.u8string();
    return std::string(s.begin(), s.end());
}

}  // namespace studio::detail

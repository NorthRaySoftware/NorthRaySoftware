#include "cjui/icons.hpp"

#include <algorithm>
#include <cmath>
#include <cwctype>
#include <utility>
#include <vector>

namespace cjui {

namespace {
constexpr float kPi = 3.14159265f;
}

void drawIcon(Gfx& g, Icon icon, const Rect& box, Color c, float width) {
    if (icon == Icon::None) return;
    const float s = std::min(box.w, box.h) / 16.0f;
    const float ox = box.x + (box.w - 16 * s) / 2, oy = box.y + (box.h - 16 * s) / 2;
    const float w = width * std::max(0.75f, s);
    auto X = [&](float v) { return ox + v * s; };
    auto Y = [&](float v) { return oy + v * s; };
    auto L = [&](float x1, float y1, float x2, float y2) { g.line(X(x1), Y(y1), X(x2), Y(y2), c, w); };
    auto R = [&](float x, float y, float ww, float hh, float rad) { g.strokeRounded({X(x), Y(y), ww * s, hh * s}, rad * s, c, w); };
    auto F = [&](float x, float y, float ww, float hh, float rad) { g.fillRounded({X(x), Y(y), ww * s, hh * s}, rad * s, c); };
    auto C = [&](float cx, float cy, float r) { g.circle(X(cx), Y(cy), r * s, c, false, w); };
    auto D = [&](float cx, float cy, float r) { g.circle(X(cx), Y(cy), r * s, c, true); };
    auto A = [&](float cx, float cy, float r, float a0, float a1) { g.arc(X(cx), Y(cy), r * s, a0, a1, c, w); };
    switch (icon) {
        case Icon::None: break;
        case Icon::Library:   // books on a shelf
            R(2, 3, 3, 10, 0.8f), R(6, 3, 3, 10, 0.8f);
            L(10.5f, 4, 13.5f, 3.2f), L(13.5f, 3.2f, 15, 12.6f), L(15, 12.6f, 12, 13.4f), L(12, 13.4f, 10.5f, 4);
            L(1, 14.5f, 15, 14.5f);
            break;
        case Icon::Writing:   // a pen nib writing a line
            L(10.5f, 2.5f, 13.5f, 5.5f), L(13.5f, 5.5f, 6, 13), L(6, 13, 2.5f, 13.5f), L(2.5f, 13.5f, 3, 10), L(3, 10, 10.5f, 2.5f);
            L(9, 4, 12, 7);
            break;
        case Icon::Planning:   // cards on a board
            R(1.5f, 2.5f, 5.5f, 4.5f, 1), R(9, 2.5f, 5.5f, 4.5f, 1), R(1.5f, 9, 5.5f, 4.5f, 1), R(9, 9, 5.5f, 4.5f, 1);
            break;
        case Icon::World:   // a globe
            C(8, 8, 6);
            L(2, 8, 14, 8);
            {
                // An ellipse for the meridian: a few short lines.
                const int n = 16;
                for (int i = 0; i < n; ++i) {
                    const float a0 = kPi * 2 * i / n, a1 = kPi * 2 * (i + 1) / n;
                    L(8 + 2.6f * std::cos(a0), 8 + 6 * std::sin(a0), 8 + 2.6f * std::cos(a1), 8 + 6 * std::sin(a1));
                }
            }
            break;
        case Icon::Review:   // a magnifier over lines
            L(2, 4, 8, 4), L(2, 7, 6, 7), L(2, 10, 5, 10);
            C(10, 9.5f, 3.2f);
            L(12.3f, 11.8f, 14.5f, 14);
            break;
        case Icon::Typeset:   // a page with justified lines
            R(3, 1.5f, 10, 13, 1);
            L(5, 4.5f, 11, 4.5f), L(5, 7, 11, 7), L(5, 9.5f, 11, 9.5f), L(5, 12, 9, 12);
            break;
        case Icon::Preview:   // a reader in a device
            R(4, 1.5f, 8, 13, 1.5f);
            L(6, 4.5f, 10, 4.5f), L(6, 7, 10, 7), L(6, 9.5f, 9, 9.5f);
            D(8, 12.5f, 0.7f);
            break;
        case Icon::Publish:   // a book with an arrow going up out of it
            R(2.5f, 7, 11, 7.5f, 1);
            L(8, 1.5f, 8, 10), L(5, 4.5f, 8, 1.5f), L(8, 1.5f, 11, 4.5f);
            break;
        case Icon::Settings: {   // a gear
            C(8, 8, 2.2f);
            const int teeth = 8;
            for (int i = 0; i < teeth; ++i) {
                const float a = kPi * 2 * i / teeth;
                L(8 + 4.2f * std::cos(a), 8 + 4.2f * std::sin(a), 8 + 6.2f * std::cos(a), 8 + 6.2f * std::sin(a));
            }
            C(8, 8, 4.4f);
            break;
        }
        case Icon::Search: C(7, 7, 4.5f), L(10.3f, 10.3f, 14, 14); break;
        case Icon::Plus: L(8, 3, 8, 13), L(3, 8, 13, 8); break;
        case Icon::Trash:
            L(2.5f, 4, 13.5f, 4), L(6, 4, 6.5f, 2), L(6.5f, 2, 9.5f, 2), L(9.5f, 2, 10, 4);
            L(4, 4, 5, 14), L(5, 14, 11, 14), L(11, 14, 12, 4);
            L(6.8f, 6.5f, 7, 11.5f), L(9.2f, 6.5f, 9, 11.5f);
            break;
        case Icon::Import:   // an arrow down into a tray
            L(8, 1.5f, 8, 10), L(5, 7, 8, 10), L(8, 10, 11, 7);
            L(2, 10, 2, 14), L(2, 14, 14, 14), L(14, 14, 14, 10);
            break;
        case Icon::Export:   // an arrow up out of a tray
            L(8, 10, 8, 1.5f), L(5, 4.5f, 8, 1.5f), L(8, 1.5f, 11, 4.5f);
            L(2, 10, 2, 14), L(2, 14, 14, 14), L(14, 14, 14, 10);
            break;
        case Icon::Open:   // a box with an arrow out of its corner
            L(7, 3, 3, 3), L(3, 3, 3, 13), L(3, 13, 13, 13), L(13, 13, 13, 9);
            L(8, 8, 13.5f, 2.5f), L(9.5f, 2.5f, 13.5f, 2.5f), L(13.5f, 2.5f, 13.5f, 6.5f);
            break;
        case Icon::Pencil:
            L(10.5f, 2.5f, 13.5f, 5.5f), L(13.5f, 5.5f, 5.5f, 13.5f), L(5.5f, 13.5f, 2.5f, 13.5f), L(2.5f, 13.5f, 2.5f, 10.5f),
                L(2.5f, 10.5f, 10.5f, 2.5f);
            break;
        case Icon::Back: L(10, 3, 5, 8), L(5, 8, 10, 13); break;
        case Icon::Forward: L(6, 3, 11, 8), L(11, 8, 6, 13); break;
        case Icon::Share:
            C(4, 8, 2), C(12, 3.5f, 2), C(12, 12.5f, 2);
            L(5.8f, 7, 10.2f, 4.5f), L(5.8f, 9, 10.2f, 11.5f);
            break;
        case Icon::Picture:
            R(1.5f, 2.5f, 13, 11, 1.5f);
            C(5.5f, 6, 1.4f);
            L(2.5f, 12, 7, 8), L(7, 8, 9.5f, 10.5f), L(9.5f, 10.5f, 11, 9), L(11, 9, 13.5f, 11.5f);
            break;
        case Icon::Book:
            L(8, 4, 8, 14);
            L(8, 4, 2, 2.5f), L(2, 2.5f, 2, 12.5f), L(2, 12.5f, 8, 14);
            L(8, 4, 14, 2.5f), L(14, 2.5f, 14, 12.5f), L(14, 12.5f, 8, 14);
            break;
        case Icon::Close: L(3.5f, 3.5f, 12.5f, 12.5f), L(12.5f, 3.5f, 3.5f, 12.5f); break;
        case Icon::Undo:
            A(8.5f, 9, 4.5f, -kPi * 0.95f, kPi * 0.55f);
            L(4, 9, 4, 4.5f), L(4, 9, 8.2f, 8.4f);
            break;
        case Icon::Check: L(2.5f, 8.5f, 6.5f, 12.5f), L(6.5f, 12.5f, 13.5f, 3.5f); break;
        case Icon::Folder:
            L(1.5f, 4, 1.5f, 13), L(1.5f, 13, 14.5f, 13), L(14.5f, 13, 14.5f, 5.5f), L(14.5f, 5.5f, 7.5f, 5.5f), L(7.5f, 5.5f, 6, 3.5f),
                L(6, 3.5f, 1.5f, 3.5f);
            break;
        case Icon::Sync:
            A(8, 8, 5.5f, -kPi * 0.9f, -kPi * 0.1f), A(8, 8, 5.5f, kPi * 0.1f, kPi * 0.9f);
            L(13.3f, 3.5f, 13.3f, 6.8f), L(13.3f, 6.8f, 10.2f, 6.2f);
            L(2.7f, 12.5f, 2.7f, 9.2f), L(2.7f, 9.2f, 5.8f, 9.8f);
            break;
        case Icon::Copy: R(5, 5, 9, 9, 1.5f), L(3, 10.5f, 2, 10.5f), L(2, 10.5f, 2, 2), L(2, 2, 10.5f, 2), L(10.5f, 2, 10.5f, 3); break;
        case Icon::More: D(3.5f, 8, 1.3f), D(8, 8, 1.3f), D(12.5f, 8, 1.3f); break;
        case Icon::Up: L(8, 13, 8, 3), L(4, 7, 8, 3), L(8, 3, 12, 7); break;
        case Icon::Down: L(8, 3, 8, 13), L(4, 9, 8, 13), L(8, 13, 12, 9); break;
        case Icon::Comment:
            R(1.5f, 2.5f, 13, 9, 2);
            L(5, 11.5f, 4, 14.5f), L(4, 14.5f, 8, 11.5f);
            break;
        case Icon::Person: C(8, 5, 3), A(8, 15, 6, -kPi * 0.95f, -kPi * 0.05f); break;
        case Icon::Pin:
            C(8, 6, 4);
            L(5, 9, 8, 14.5f), L(8, 14.5f, 11, 9);
            D(8, 6, 1.3f);
            break;
        case Icon::Link:
            R(1.5f, 5.5f, 7.5f, 5, 2.5f), R(7, 5.5f, 7.5f, 5, 2.5f);
            break;
        case Icon::Eye:
            A(8, 13, 8, -kPi * 0.82f, -kPi * 0.18f), A(8, 3, 8, kPi * 0.18f, kPi * 0.82f);
            C(8, 8, 2);
            break;
        case Icon::Font:
            L(2, 13, 6, 3), L(6, 3, 10, 13), L(3.6f, 9, 8.4f, 9);
            C(12, 10.5f, 2.3f), L(14.3f, 8, 14.3f, 13);
            break;
        case Icon::Archive:
            R(1.5f, 2.5f, 13, 3.5f, 1);
            L(2.5f, 6, 2.5f, 13.5f), L(2.5f, 13.5f, 13.5f, 13.5f), L(13.5f, 13.5f, 13.5f, 6);
            L(6, 9, 10, 9);
            break;
        case Icon::Camera:
            R(1.5f, 4.5f, 13, 9, 1.5f);
            L(5.5f, 4.5f, 6.5f, 2.5f), L(6.5f, 2.5f, 9.5f, 2.5f), L(9.5f, 2.5f, 10.5f, 4.5f);
            C(8, 9, 2.6f);
            break;
        case Icon::Target: C(8, 8, 6), C(8, 8, 3), D(8, 8, 1); break;
        case Icon::Filter: L(1.5f, 3, 14.5f, 3), L(1.5f, 3, 6.5f, 8.5f), L(14.5f, 3, 9.5f, 8.5f), L(6.5f, 8.5f, 6.5f, 13.5f), L(9.5f, 8.5f, 9.5f, 12), L(6.5f, 13.5f, 9.5f, 12); break;
        case Icon::Sort: L(4.5f, 2.5f, 4.5f, 13.5f), L(2, 11, 4.5f, 13.5f), L(4.5f, 13.5f, 7, 11), L(11.5f, 13.5f, 11.5f, 2.5f), L(9, 5, 11.5f, 2.5f), L(11.5f, 2.5f, 14, 5); break;
        case Icon::Panel: R(1.5f, 2.5f, 13, 11, 1.5f), L(10, 2.5f, 10, 13.5f); break;
        case Icon::Sidebar: R(1.5f, 2.5f, 13, 11, 1.5f), L(6, 2.5f, 6, 13.5f); break;
        case Icon::Cards: R(1.5f, 1.5f, 5.5f, 5.5f, 1), R(9, 1.5f, 5.5f, 5.5f, 1), R(1.5f, 9, 5.5f, 5.5f, 1), R(9, 9, 5.5f, 5.5f, 1); break;
        case Icon::List: L(5, 4, 14, 4), L(5, 8, 14, 8), L(5, 12, 14, 12), D(2.2f, 4, 1), D(2.2f, 8, 1), D(2.2f, 12, 1); break;
        case Icon::Map:
            L(1.5f, 3.5f, 5.5f, 2), L(5.5f, 2, 10.5f, 3.5f), L(10.5f, 3.5f, 14.5f, 2), L(14.5f, 2, 14.5f, 12.5f), L(14.5f, 12.5f, 10.5f, 14),
                L(10.5f, 14, 5.5f, 12.5f), L(5.5f, 12.5f, 1.5f, 14), L(1.5f, 14, 1.5f, 3.5f), L(5.5f, 2, 5.5f, 12.5f), L(10.5f, 3.5f, 10.5f, 14);
            break;
        case Icon::Spark:
            L(8, 1.5f, 8, 5), L(8, 11, 8, 14.5f), L(1.5f, 8, 5, 8), L(11, 8, 14.5f, 8);
            L(3.5f, 3.5f, 5.5f, 5.5f), L(10.5f, 10.5f, 12.5f, 12.5f), L(12.5f, 3.5f, 10.5f, 5.5f), L(5.5f, 10.5f, 3.5f, 12.5f);
            break;
    }
    (void)F;
}

Icon iconFor(const std::wstring& label) {
    std::wstring t;
    for (wchar_t ch : label) t += (wchar_t)std::towlower(ch);
    auto has = [&](const wchar_t* k) { return t.find(k) != std::wstring::npos; };
    auto starts = [&](const wchar_t* k) { return t.rfind(k, 0) == 0; };
    if (starts(L"‹") || starts(L"back")) return Icon::Back;
    if (starts(L"+") || starts(L"new ") || starts(L"add ") || t == L"new" || t == L"add" || starts(L"make a ") || starts(L"make my own")) return Icon::Plus;
    if (has(L"trash") || starts(L"delete") || starts(L"remove")) return Icon::Trash;
    if (starts(L"restore") || starts(L"undo")) return Icon::Undo;
    if (starts(L"import")) return Icon::Import;
    if (starts(L"export") || starts(L"save as") || starts(L"download")) return Icon::Export;
    if (starts(L"rename") || starts(L"edit")) return Icon::Pencil;
    if (has(L"settings")) return Icon::Settings;
    if (starts(L"share")) return Icon::Share;
    if (has(L"github") || starts(L"sync")) return Icon::Sync;
    if (starts(L"search") || starts(L"find")) return Icon::Search;
    if (starts(L"copy") || starts(L"duplicate")) return Icon::Copy;
    if (has(L"picture") || has(L"cover") || has(L"image")) return Icon::Picture;
    if (starts(L"back up") || has(L"backup")) return Icon::Archive;
    if (has(L"comment")) return Icon::Comment;
    if (has(L"font")) return Icon::Font;
    if (has(L"map") || has(L"pin")) return Icon::Map;
    if (has(L"family")) return Icon::Person;
    if (starts(L"show") || has(L"preview")) return Icon::Eye;
    if (starts(L"open in writing") || starts(L"write")) return Icon::Writing;
    if (starts(L"open")) return Icon::Open;
    if (has(L"snapshot")) return Icon::Camera;
    if (starts(L"done") || starts(L"use ") || t == L"ok" || starts(L"resolve") || starts(L"it's fine")) return Icon::Check;
    if (starts(L"close") || starts(L"cancel") || starts(L"stop")) return Icon::Close;
    if (has(L"move up") || has(L"earlier")) return Icon::Up;
    if (has(L"move down") || has(L"later")) return Icon::Down;
    if (starts(L"move")) return Icon::Folder;
    if (has(L"pdf") || has(L"print")) return Icon::Typeset;
    if (has(L"epub") || has(L"ebook")) return Icon::Book;
    return Icon::None;
}

}  // namespace cjui

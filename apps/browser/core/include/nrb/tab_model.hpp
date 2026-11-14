// The order of a window's tabs, which one is active, and recently closed tabs.
// Holds no engine objects: the Windows layer maps tab ids to browsers. Kept
// separate so tab behaviour (where a new tab goes, which tab becomes active
// when one closes) is unit-tested without a browser engine.
#pragma once

#include <cstddef>
#include <deque>
#include <string>
#include <vector>

namespace nrb {

struct Tab {
    int id = 0;
    std::string url;
    std::string title;
    bool loading = false;
    bool pinned = false;        // reserved for 0.2: pinned tabs stay at the left
    int openerId = 0;           // the tab that opened this one (0 if none)
};

struct ClosedTab {
    std::string url;
    std::string title;
    std::size_t index = 0;
};

class TabModel {
public:
    // A new tab. `afterActive` puts it just after the active tab (links and
    // pop-ups); otherwise it goes at the end (Ctrl+T, the + button).
    int add(const std::string& url, bool activate, bool afterActive, int openerId = 0);

    // Removes the tab. When it was active, the tab to its right becomes active
    // (or to its left if it was last). Returns false if there's no such tab.
    // `remember` puts it on the recently-closed list (if it had an address).
    bool close(int id, bool remember = true);

    bool activate(int id);
    // Cyclic: +1 the next tab, -1 the previous one (Ctrl+Tab, Ctrl+Shift+Tab).
    void activateRelative(int delta);
    // Ctrl+1..8: that position; Ctrl+9: the last tab.
    void activateIndex(std::size_t index);
    bool move(int id, std::size_t toIndex);

    Tab* find(int id);
    const Tab* find(int id) const;
    Tab* active();
    int activeId() const { return activeId_; }
    int indexOf(int id) const;  // -1 if absent
    const std::vector<Tab>& tabs() const { return tabs_; }
    std::size_t size() const { return tabs_.size(); }
    bool empty() const { return tabs_.empty(); }

    // Most recent first; at most kMaxClosed are kept.
    bool hasClosed() const { return !closed_.empty(); }
    ClosedTab popClosed();
    static constexpr std::size_t kMaxClosed = 25;

private:
    std::vector<Tab> tabs_;
    std::deque<ClosedTab> closed_;
    int activeId_ = 0;
    int nextId_ = 1;
};

}  // namespace nrb

#include "nrb/tab_model.hpp"

#include <algorithm>

namespace nrb {

int TabModel::add(const std::string& url, bool activateIt, bool afterActive, int openerId) {
    Tab t;
    t.id = nextId_++;
    t.url = url;
    t.openerId = openerId;
    std::size_t at = tabs_.size();
    if (afterActive) {
        const int a = indexOf(activeId_);
        if (a >= 0) {
            at = (std::size_t)a + 1;
            // Several links opened from the same tab go in order after it.
            while (at < tabs_.size() && openerId && tabs_[at].openerId == openerId) ++at;
        }
    }
    tabs_.insert(tabs_.begin() + (std::ptrdiff_t)at, t);
    if (activateIt || activeId_ == 0) activeId_ = t.id;
    return t.id;
}

bool TabModel::close(int id, bool remember) {
    const int i = indexOf(id);
    if (i < 0) return false;
    if (remember && !tabs_[(std::size_t)i].url.empty() && tabs_[(std::size_t)i].url != "about:blank") {
        closed_.push_front({tabs_[(std::size_t)i].url, tabs_[(std::size_t)i].title, (std::size_t)i});
        if (closed_.size() > kMaxClosed) closed_.pop_back();
    }
    tabs_.erase(tabs_.begin() + i);
    for (Tab& t : tabs_)
        if (t.openerId == id) t.openerId = 0;
    if (activeId_ == id) {
        if (tabs_.empty()) activeId_ = 0;
        else activeId_ = tabs_[std::min((std::size_t)i, tabs_.size() - 1)].id;
    }
    return true;
}

bool TabModel::activate(int id) {
    if (indexOf(id) < 0) return false;
    activeId_ = id;
    return true;
}

void TabModel::activateRelative(int delta) {
    if (tabs_.empty()) return;
    const int n = (int)tabs_.size();
    const int i = std::max(0, indexOf(activeId_));
    activeId_ = tabs_[(std::size_t)(((i + delta) % n + n) % n)].id;
}

void TabModel::activateIndex(std::size_t index) {
    if (tabs_.empty()) return;
    activeId_ = tabs_[std::min(index, tabs_.size() - 1)].id;
}

bool TabModel::move(int id, std::size_t toIndex) {
    const int i = indexOf(id);
    if (i < 0) return false;
    Tab t = tabs_[(std::size_t)i];
    tabs_.erase(tabs_.begin() + i);
    toIndex = std::min(toIndex, tabs_.size());
    tabs_.insert(tabs_.begin() + (std::ptrdiff_t)toIndex, t);
    return true;
}

Tab* TabModel::find(int id) {
    for (Tab& t : tabs_)
        if (t.id == id) return &t;
    return nullptr;
}

const Tab* TabModel::find(int id) const {
    for (const Tab& t : tabs_)
        if (t.id == id) return &t;
    return nullptr;
}

Tab* TabModel::active() { return find(activeId_); }

int TabModel::indexOf(int id) const {
    for (std::size_t i = 0; i < tabs_.size(); ++i)
        if (tabs_[i].id == id) return (int)i;
    return -1;
}

ClosedTab TabModel::popClosed() {
    if (closed_.empty()) return {};
    ClosedTab c = closed_.front();
    closed_.pop_front();
    return c;
}

}  // namespace nrb

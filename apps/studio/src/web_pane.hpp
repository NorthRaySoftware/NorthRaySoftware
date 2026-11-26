#pragma once
// A WebView2 pane for the Writing Studio's editor. Pages come from the app's
// own "web" folder (https://editor.nrstudio.localhost/) and images from the
// open library's objects folder (https://objects.nrstudio.localhost/). The
// page never navigates anywhere else; links open in the normal browser.
// Messages in both directions are JSON.

#include <windows.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace nrs {

class WebPane {
public:
    // Called on the UI thread with each JSON message from the page, and with
    // {"type":"failed"} if the browser engine stops working.
    using Receiver = std::function<void(const std::string& json)>;

    WebPane();
    ~WebPane();
    WebPane(const WebPane&) = delete;
    WebPane& operator=(const WebPane&) = delete;

    bool create(HWND parent, const std::wstring& exeDir, const std::wstring& userDataDir, const std::wstring& objectsDir,
                const std::wstring& page, Receiver receiver);
    void destroy();
    // Points the image host at another library's objects folder.
    void setObjectsFolder(const std::wstring& dir);
    void place(const RECT& bounds);   // parent client pixels
    void hide();
    // The page's zoom (suite 0.172: the interface zoom, so the writing bar and text grow with the window).
    void setZoom(double factor);
    bool send(const std::string& json);  // false if not ready yet
    bool ready() const;
    bool created() const { return (bool)impl_; }
    void focus();
    const std::wstring& error() const { return error_; }
    // NorthRay's own items at the top of the page's right-click menu, worked out when it opens from the
    // text selected (if any). Each is a label and what choosing it does.
    using ContextItems = std::function<std::vector<std::pair<std::wstring, std::function<void()>>>(const std::wstring& selection)>;
    void setContextItems(ContextItems items);

    struct Impl;

private:
    std::shared_ptr<Impl> impl_;
    std::wstring error_;
};

}  // namespace nrs

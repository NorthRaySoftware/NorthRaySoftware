// NorthRay Capture: diagnostics rules (system 15). Portable and unit-tested.
//  - redact: every log line passes through it, so stream keys, tokens and passwords never reach a
//    log file or a diagnostics export.
//  - layout: the workspace's panels (which, in what order, how big), saved and restored.
#pragma once

#include <string>
#include <vector>

namespace ncap {

// Hides secrets in text: each of `secrets` (stream keys the app knows) wherever it appears; the
// last part of rtmp:// and rtmps:// addresses that carries a key; query values named key, token,
// auth, password, secret, sig or signature; "Bearer ..." tokens; and long key-like runs after
// "live_" or "sk_". What's hidden becomes "•••".
std::string redact(const std::string& text, const std::vector<std::string>& secrets = {});

// The workspace: a preview over a row of docked panels.
struct Layout {
    enum Panel { Scenes = 0, Sources, Mixer, Controls, Chat, Soundboard, Notes, Writing, kPanels };   // Chat, Soundboard, Notes, Writing: hidden at first
    std::vector<int> order = {Scenes, Sources, Mixer, Controls};   // left to right; hidden panels aren't listed
    std::vector<float> widths = {0.2f, 0.25f, 0.35f, 0.2f, 0.22f, 0.22f, 0.25f, 0.2f};  // by panel, fractions of the row
    float dockHeight = 0.34f;                                        // the panel row, as a fraction of the space
    bool previewVisible = true;

    // Keeps it sane: every listed panel once, known panels only, at least one panel, sizes within
    // limits and the visible ones' widths summing to 1.
    void normalise();
    bool shows(int panel) const;
    void hide(int panel);
    void show(int panel);   // at the end of the row
    void move(int panel, int delta);
    // Moves the divider after the `index`-th shown panel by `fraction` of the row (taking it from the next).
    void resizeDivider(int index, float fraction);
    // Where the shown panels go in a dock `w` by `h` (DIPs, from 0,0; `gap` between them), suite 0.176: one row at
    // their widths when each gets at least `minW`; on a narrower dock they wrap into rows (as few as fit at `minW`,
    // filled evenly, in order), the rows sharing the height and each row's panels sharing its width by their widths.
    struct Box { int panel = 0; float x = 0, y = 0, w = 0, h = 0; };
    struct Arrangement { std::vector<Box> boxes; int rows = 1; bool wrapped = false; };
    Arrangement arrange(float w, float h, float gap, float minW) const;
    std::string toJson() const;
    static Layout fromJson(const std::string& json);   // the default layout if it isn't one
    static const char* panelName(int panel);
};

}  // namespace ncap

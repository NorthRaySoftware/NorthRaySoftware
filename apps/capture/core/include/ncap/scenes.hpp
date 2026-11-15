// NorthRay Capture: scenes and sources as data (system 4). Portable and unit-tested; the capture,
// compositor and UI layers read and change it.
//
// A *source* is one thing that produces pictures or sound (a monitor, a window, a camera, an image,
// an application's audio). A *scene* is an ordered list of *items*, each placing a source on the
// canvas with its own transform. Several scenes can show the same source: the source is captured
// once and shared, so switching scenes never restarts a capture.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ncap {

using Id = std::string;   // "s12": stable across renames and saves

enum class SourceKind {
    Display,          // a whole monitor
    Window,           // one application's window
    Region,           // part of a monitor
    Camera,           // webcam, USB or HDMI capture card (video)
    Image,
    Media,            // a video or sound file
    Text,
    Colour,
    Browser,
    Slideshow,        // the images in a folder, one after another
    Scene,            // another scene, nested
    InputAudio,       // microphone, capture card's audio, line in
    DesktopAudio,     // everything an output device plays (loopback), optionally minus some apps
    ApplicationAudio, // one application's sound (process loopback)
};
const char* kindKey(SourceKind k);                       // "display", "app-audio"...
std::optional<SourceKind> kindFromKey(const std::string& key);
bool isAudioOnly(SourceKind k);
bool hasSound(SourceKind k);   // sound sources and media files: a channel in the mixer

struct Transform {
    float x = 0, y = 0;            // the item's top-left on the canvas, in canvas pixels
    float width = 0, height = 0;   // its size on the canvas (0 = the source's own size)
    float rotation = 0;            // degrees, clockwise, about the centre
    float cropLeft = 0, cropTop = 0, cropRight = 0, cropBottom = 0;   // source pixels
    float opacity = 1;
    bool flipH = false, flipV = false;   // mirrored left-right / upside down (before rotation)
};

struct Source {
    Id id;
    SourceKind kind = SourceKind::Colour;
    std::string name;
    std::string settings;   // the kind's own settings, as JSON (which monitor, which file...)
};

struct SceneItem {
    Id id;
    Id source;
    Transform transform;
    bool visible = true;
    bool locked = false;
};

struct Scene {
    Id id;
    std::string name;
    std::string folder;             // "" = not in a folder; the Scenes list groups by it
    std::vector<SceneItem> items;   // bottom first; the last is drawn on top
};

class SceneCollection {
public:
    SceneCollection();   // one scene, "Scene"

    int canvasWidth = 1920, canvasHeight = 1080;

    const std::vector<Scene>& scenes() const { return scenes_; }
    const std::vector<Source>& sources() const { return sources_; }
    const Scene* scene(const Id& id) const;
    const Source* source(const Id& id) const;
    const Id& current() const { return current_; }
    bool setCurrent(const Id& id);

    // Scenes. Names are unique (any case); a taken or empty name gets a number ("Scene 2").
    Id addScene(const std::string& name);
    bool renameScene(const Id& id, const std::string& name);   // false if empty or taken by another
    Id duplicateScene(const Id& id);                            // "Gaming (copy)": the same sources, shared
    bool removeScene(const Id& id);                             // never the last one
    bool moveScene(const Id& id, int delta);                    // up (-1) or down (+1) the list
    std::string uniqueSceneName(const std::string& wanted) const;

    // Sources and items.
    Id addSource(SourceKind kind, const std::string& name, const std::string& settings = "{}");
    Id addItem(const Id& scene, const Id& source, const Transform& t = {});
    bool removeItem(const Id& scene, const Id& item);
    // Picture sources no scene shows any more (to stop capturing them). Sound sources are never
    // in this list: they're heard in every scene.
    std::vector<Id> unusedSources() const;
    void removeSource(const Id& id);   // and every item showing it
    bool renameSource(const Id& id, const std::string& name);   // false if empty or taken by another
    bool setSourceSettings(const Id& id, const std::string& settings);

    // Nesting: a Scene source shows another scene, as it is, inside this one.
    Id sceneSource(const Id& scene);                        // the source showing that scene (made if needed); "" if no such scene
    static Id nestedScene(const Source& s);                 // the scene a Scene source shows, "" for other kinds
    bool wouldLoop(const Id& into, const Id& nested) const; // showing `nested` inside `into` would show a scene inside itself
    // Copy an item (its own transform; the same source, shared).
    Id duplicateItem(const Id& scene, const Id& item);

    // Groups: items moved together into a scene of their own (in the "Groups" folder), shown in
    // their place by one item, so they move, resize and hide as one. Their placements are kept (a
    // group's scene is canvas-sized, and its item starts covering the canvas exactly).
    Id groupItems(const Id& scene, const std::vector<Id>& items, std::string& error);   // the group's item
    bool addToGroup(const Id& scene, const Id& groupItem, const Id& item, std::string& error);
    // The items back in the scene, placed where they showed. Not when the group is rotated, cropped
    // or mirrored (that can't be put back exactly): false with `error`.
    bool ungroup(const Id& scene, const Id& groupItem, std::string& error);
    bool isGroupItem(const Id& scene, const Id& item) const;

    const SceneItem* item(const Id& scene, const Id& item) const;
    bool setTransform(const Id& scene, const Id& item, const Transform& t);
    bool setItemVisible(const Id& scene, const Id& item, bool visible);
    bool setItemLocked(const Id& scene, const Id& item, bool locked);
    // Towards the top of the scene (+1, drawn later) or the bottom (-1); +100 / -100 to the very top / bottom.
    bool moveItem(const Id& scene, const Id& item, int delta);

    // Folders: a name ("" takes it out of its folder). Scenes in a folder are kept together in the
    // list, in the folder's place.
    bool setSceneFolder(const Id& scene, const std::string& folder);

    // A scene as a file of its own: the scene, the scenes shown inside it, and the sources they show
    // (with their settings: files' paths, which display... never stream keys, which aren't in here).
    std::string exportScene(const Id& scene) const;
    // Adds an exported scene (and the scenes inside it) with new ids and names that don't clash;
    // placements are scaled if it was made for another canvas size. The new scene, or "" with `error`.
    Id importScene(const std::string& text, std::string& error, bool* scaled = nullptr);

    std::string toJson() const;
    // False (and the collection unchanged) if it isn't one; a newer version's file is read as far
    // as it's understood.
    bool fromJson(const std::string& json);
    std::uint64_t changes() const { return changes_; }

private:
    std::vector<Scene> scenes_;
    std::vector<Source> sources_;
    Id current_;
    std::uint64_t nextId_ = 1;
    std::uint64_t changes_ = 0;
    Id newId(char prefix);
    Scene* sceneMut(const Id& id);
};

}  // namespace ncap

// Unit tests for NorthRay Capture's portable core. Dependency-free like the other suites.
#include "ncap/audio.hpp"
#include "ncap/diag.hpp"
#include "ncap/placement.hpp"
#include "ncap/routing.hpp"

#include <cmath>
#include "ncap/scenes.hpp"

#include <cstdio>
#include <string>

using namespace ncap;

static int g_failed = 0, g_passed = 0;
#define CHECK(cond)                                                                        \
    do {                                                                                   \
        if (cond) ++g_passed;                                                              \
        else { ++g_failed; std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } \
    } while (0)

static void testItemEditing() {
    std::printf("item editing\n");
    SceneCollection c;
    const Id sc = c.current();
    const Id a = c.addSource(SourceKind::Display, "Display");
    const Id b = c.addSource(SourceKind::Camera, "Webcam");
    const Id ia = c.addItem(sc, a), ib = c.addItem(sc, b);
    CHECK(!c.renameSource(b, " display ") && !c.renameSource(b, "  ") && c.renameSource(b, " Face cam "));
    CHECK(c.source(b)->name == "Face cam");
    Transform t;
    t.x = 10;
    t.flipH = true;
    const auto before = c.changes();
    CHECK(c.setTransform(sc, ia, t) && c.changes() == before + 1);
    CHECK(c.setTransform(sc, ia, t) && c.changes() == before + 1);   // unchanged: no new change
    CHECK(c.setItemVisible(sc, ib, false) && !c.item(sc, ib)->visible);
    CHECK(c.setItemLocked(sc, ia, true) && c.item(sc, ia)->locked);
    CHECK(c.moveItem(sc, ia, 1) && c.scene(sc)->items.back().id == ia);   // to the top
    CHECK(!c.moveItem(sc, ia, 5));                                         // already there
    CHECK(c.moveItem(sc, ia, -100) && c.scene(sc)->items.front().id == ia);
    CHECK(c.setSourceSettings(a, "{\"monitor\":1}") && !c.setSourceSettings("nope", "{}"));
    c.addSource(SourceKind::InputAudio, "Microphone");   // sound: in no scene, yet in use
    CHECK(c.unusedSources().empty());
    SceneCollection d;
    CHECK(d.fromJson(c.toJson()));
    const SceneItem* back = d.item(sc, ia);
    CHECK(back && back->transform.x == 10 && back->transform.flipH && !back->transform.flipV && back->locked);
    CHECK(d.source(b)->name == "Face cam" && !d.item(sc, ib)->visible);
}

static bool near(float a, float b, float eps = 0.01f) { return std::fabs(a - b) <= eps; }

static void testPlacement() {
    std::printf("placement\n");
    Transform t;
    // The source's own size, less the crop, when the transform has none.
    t.cropLeft = 100;
    t.cropRight = 20;
    Box f = frameOf(t, 1920, 1080);
    CHECK(near(f.w, 1800) && near(f.h, 1080));
    CHECK(frameOf(Transform{}, 0, 0).w == 0);   // nothing captured yet: no size
    t = {};
    t.x = 100;
    t.y = 50;
    t.width = 400;
    t.height = 200;
    CHECK(hits(t, 0, 0, {110, 60}) && !hits(t, 0, 0, {90, 60}) && !hits(t, 0, 0, {510, 60}));
    // Rotated a quarter turn about its centre (300, 150): it now stands 200 wide, 400 tall.
    Transform r = rotateBy(t, 0, 0, 90);
    Box b = boundsOf(r, 0, 0);
    CHECK(near(b.x, 200) && near(b.y, -50) && near(b.w, 200) && near(b.h, 400));
    CHECK(hits(r, 0, 0, {300, 0}) && !hits(r, 0, 0, {120, 150}));
    CHECK(near(rotateBy(r, 0, 0, -180).rotation, 270) && near(rotateBy(r, 0, 0, 270).rotation, 0));

    // Topmost visible item under the point.
    SceneItem a, c;
    a.id = "a";
    a.transform = t;
    c.id = "c";
    c.transform = t;
    c.transform.x = 300;
    std::vector<SceneItem> items{a, c};
    std::vector<ItemSize> sizes{{0, 0}, {0, 0}};
    CHECK(itemAt(items, sizes, {350, 100}) == "c" && itemAt(items, sizes, {150, 100}) == "a" && itemAt(items, sizes, {5, 5}).empty());
    items[1].visible = false;
    CHECK(itemAt(items, sizes, {350, 100}) == "a");

    // Handles.
    CHECK(handleAt(t, 0, 0, {102, 52}, 8) == Handle::TopLeft);
    CHECK(handleAt(t, 0, 0, {300, 250}, 8) == Handle::Bottom);
    CHECK(handleAt(t, 0, 0, {300, 150}, 8) == Handle::None);
    CHECK(handleCursor(Handle::Right, 0) == 0 && handleCursor(Handle::Right, 90) == 1 && handleCursor(Handle::TopLeft, 0) == 2 &&
          handleCursor(Handle::TopRight, 0) == 3 && handleCursor(Handle::TopLeft, 90) == 3);

    // Dragging the bottom-right corner keeps the shape; the top-left stays put.
    Transform g = resizeBy(t, 0, 0, Handle::BottomRight, {200, 10}, false);
    CHECK(near(g.x, 100) && near(g.y, 50) && near(g.width, 600) && near(g.height, 300));
    // Free (Shift): each way on its own.
    g = resizeBy(t, 0, 0, Handle::BottomRight, {200, 10}, true);
    CHECK(near(g.width, 600) && near(g.height, 210));
    // The left edge: the right edge stays at 500.
    g = resizeBy(t, 0, 0, Handle::Left, {-50, 99}, false);
    CHECK(near(g.x, 50) && near(g.width, 450) && near(g.height, 200) && near(g.y, 50));
    // Never inside out.
    g = resizeBy(t, 0, 0, Handle::Right, {-1000, 0}, false);
    CHECK(near(g.width, 2) && near(g.x, 100));
    // Rotated: dragging the (rotated) right edge outwards along its own direction, the left edge stays.
    Transform rot = rotateBy(t, 0, 0, 90);
    const Vec2 leftBefore = handlePoint(rot, 0, 0, Handle::Left);
    g = resizeBy(rot, 0, 0, Handle::Right, {0, 100}, false);   // the frame's right edge points down after +90°
    const Vec2 leftAfter = handlePoint(g, 0, 0, Handle::Left);
    CHECK(near(g.width, 500) && near(leftBefore.x, leftAfter.x) && near(leftBefore.y, leftAfter.y));

    // Alt+drag crops: a 1920x1080 source shown at half size (960x540 at 0,0).
    Transform half;
    half.width = 960;
    half.height = 540;
    Transform k = cropBy(half, 1920, 1080, Handle::Left, {100, 0});   // 100 frame px = 200 source px
    CHECK(near(k.cropLeft, 200) && near(k.x, 100) && near(k.width, 860) && near(k.cropRight, 0));
    k = cropBy(k, 1920, 1080, Handle::Left, {-500, 0});   // uncropping stops at the source's edge
    CHECK(near(k.cropLeft, 0) && near(k.x, 0) && near(k.width, 960));
    Transform mirrored = half;
    mirrored.flipH = true;
    k = cropBy(mirrored, 1920, 1080, Handle::Left, {100, 0});   // mirrored: the frame's left is the source's right
    CHECK(near(k.cropRight, 200) && near(k.cropLeft, 0));
    k = cropBy(half, 1920, 1080, Handle::BottomRight, {-2000, -2000});   // never below 1 source pixel
    CHECK(k.width > 0 && near(k.cropRight, 1919, 0.5f) && near(k.cropBottom, 1079, 0.5f));

    // Snapping to the canvas's edges and centre lines.
    Vec2 s = snapOffset({3, 500, 100, 80}, 1920, 1080, 8);
    CHECK(near(s.x, -3) && near(s.y, 0));
    s = snapOffset({905, 497, 100, 80}, 1920, 1080, 8);   // centre at 955 → 960; centre 537 → 540
    CHECK(near(s.x, 5) && near(s.y, 3));
    CHECK(near(snapOffset({300, 300, 100, 100}, 1920, 1080, 8).x, 0));

    // Quick placements.
    Transform q;
    q = fitToCanvas(q, 1280, 1024, 1920, 1080);   // a 5:4 picture on a 16:9 canvas: full height, centred
    CHECK(near(q.height, 1080) && near(q.width, 1350) && near(q.x, 285) && near(q.y, 0));
    Transform side = rotateBy(Transform{}, 1920, 1080, 90);
    side = fitToCanvas(side, 1920, 1080, 1920, 1080);   // on its side: 1080 tall when stood up
    b = boundsOf(side, 1920, 1080);
    CHECK(near(b.h, 1080, 0.1f) && near(b.y, 0, 0.1f) && near(b.x + b.w / 2, 960, 0.1f));
    q = stretchToCanvas(Transform{}, 640, 480, 1920, 1080);
    CHECK(near(q.width, 1920) && near(q.height, 1080) && near(q.x, 0));
    q = centreOnCanvas(Transform{}, 640, 480, 1920, 1080);
    CHECK(near(q.x, 640) && near(q.y, 300) && q.width == 0);
    Transform messy = t;
    messy.rotation = 33;
    messy.flipH = true;
    messy.opacity = 0.5f;
    q = resetTransform(messy);
    CHECK(q.x == 0 && q.width == 0 && q.rotation == 0 && !q.flipH && near(q.opacity, 0.5f));

    // Finding a window again.
    std::vector<WindowIdentity> wins{{"Notepad", "Notepad", "notepad.exe"},
                                     {"Minecraft 1.21.4 - Singleplayer", "GLFW30", "javaw.exe"},
                                     {"Minecraft 1.21.4", "GLFW30", "javaw.exe"},
                                     {"Minecraft* launcher", "Chrome_WidgetWin_1", "MinecraftLauncher.exe"}};
    CHECK(matchWindow({"Minecraft 1.21.4", "GLFW30", "javaw.exe"}, wins) == 2);
    CHECK(matchWindow({"Minecraft 1.21.4 - Multiplayer (3rd-party Server)", "GLFW30", "JAVAW.EXE"}, wins) == 1);
    CHECK(matchWindow({"Discord", "Chrome_WidgetWin_1", "Discord.exe"}, wins) == -1);
    CHECK(matchWindow({"Notepad", "", ""}, wins) == 0 && matchWindow({"Something", "", ""}, wins) == -1);
}

static void testAudio() {
    std::printf("audio\n");
    using namespace ncap::audio;
    CHECK(near(dbToGain(0), 1) && near(dbToGain(-6.0206f), 0.5f, 0.001f) && dbToGain(-100) == 0 && dbToGain(-120) == 0);
    CHECK(near(gainToDb(1), 0) && near(gainToDb(0.5f), -6.02f) && gainToDb(0) == kSilenceDb);
    CHECK(faderToGain(1) == 1 && faderToGain(0) == 0 && near(gainToDb(faderToGain(0.5f)), -18.06f, 0.05f));
    CHECK(near(gainToFader(faderToGain(0.37f)), 0.37f, 0.0001f) && gainToFader(0) == 0 && gainToFader(4) == 1);
    CHECK(dbText(-0.01f) == L"0.0 dB" && dbText(-6.04f) == L"-6.0 dB" && dbText(-100) == L"-∞ dB");

    float block[8] = {0.1f, -0.7f, -0.3f, 0.2f, 0.25f, 0.0f, 0.0f, 0.05f};
    CHECK(near(peakOf(block, 4, 0), 0.3f) && near(peakOf(block, 4, 1), 0.7f));
    float dst[8] = {};
    mixInto(dst, block, 8, 0.5f);
    mixInto(dst, block, 8, 0.5f);
    CHECK(near(dst[1], -0.7f) && near(dst[2], -0.3f));

    // The meter: up at once, down at 20 dB/s, the hold line for 1.5 s.
    Meter m;
    m.feed(1.0f, 0);
    CHECK(near(m.level(0), 0) && m.clipped(1.0) && !m.clipped(2.0));
    m.feed(0.1f, 0.5);   // -20 dB arrives while the level has only fallen to -10
    CHECK(near(m.level(0.5), -10) && near(m.hold(0.5), 0));
    CHECK(near(m.level(1.0), -20) && near(m.hold(1.4), 0) && near(m.hold(1.6), m.level(1.6)));
    CHECK(m.level(10) == kSilenceDb);

    // The ring: in order, wrapping round, never more than there's room for.
    Ring r(4);
    const float a[6] = {1, 1, 2, 2, 3, 3};
    CHECK(r.write(a, 3) == 3 && r.available() == 3);
    float out[8] = {};
    CHECK(r.read(out, 2) == 2 && out[0] == 1 && out[2] == 2 && r.available() == 1);
    const float b[6] = {4, 4, 5, 5, 6, 6};
    CHECK(r.write(b, 3) == 3 && r.available() == 4);   // wraps
    CHECK(r.write(b, 1) == 0);                          // full
    CHECK(r.read(out, 8) == 4 && out[0] == 3 && out[2] == 4 && out[6] == 6);
    CHECK(r.writeSilence(2) == 2);
    r.skip(1);
    CHECK(r.available() == 1 && r.read(out, 1) == 1 && out[0] == 0);

    const auto h = wavHeader(48000, 2, 3840);
    CHECK(h.size() == 44 && h[0] == 'R' && h[8] == 'W' && h[20] == 3 && h[22] == 2 && h[34] == 32 && h[40] == 0x00 && h[41] == 0x0F);
}

static void testScenes() {
    std::printf("scenes\n");
    SceneCollection c;
    CHECK(c.scenes().size() == 1 && c.scenes()[0].name == "Scene" && c.current() == c.scenes()[0].id);
    const Id gaming = c.addScene("Gaming");
    CHECK(c.addScene("gaming") != gaming && c.scenes().back().name == "gaming 2");   // names are unique, any case
    CHECK(c.uniqueSceneName("  ") == "Scene 2");
    CHECK(!c.renameScene(gaming, "SCENE") && !c.renameScene(gaming, "   "));       // taken; empty
    CHECK(c.renameScene(gaming, " Just Chatting ") && c.scene(gaming)->name == "Just Chatting");

    // A source shown in two scenes is one source (captured once).
    const Id cam = c.addSource(SourceKind::Camera, "Webcam");
    const Id cam2 = c.addSource(SourceKind::Camera, "webcam");
    CHECK(c.source(cam2)->name == "webcam 2");
    Transform t;
    t.x = 100;
    t.width = 640;
    t.height = 360;
    const Id item = c.addItem(gaming, cam, t);
    CHECK(!item.empty());
    const Id copy = c.duplicateScene(gaming);
    CHECK(c.scene(copy)->name == "Just Chatting (copy)" && c.scene(copy)->items.size() == 1);
    CHECK(c.scene(copy)->items[0].source == cam && c.scene(copy)->items[0].id != item);
    CHECK(c.scenes()[2].id == copy);   // right after the original
    CHECK(c.unusedSources().size() == 1 && c.unusedSources()[0] == cam2);

    CHECK(c.moveScene(copy, -1) && c.scenes()[1].id == copy && !c.moveScene(c.scenes()[0].id, -1));
    CHECK(c.setCurrent(copy) && c.current() == copy);
    CHECK(c.removeScene(copy) && c.current() != copy && c.scene(c.current()) != nullptr);

    // Saved and read back.
    const std::string json = c.toJson();
    SceneCollection r;
    CHECK(r.fromJson(json));
    CHECK(r.scenes().size() == c.scenes().size() && r.sources().size() == 2);
    CHECK(r.scene(gaming) && r.scene(gaming)->items.size() == 1 && r.scene(gaming)->items[0].transform.width == 640 &&
          r.scene(gaming)->items[0].transform.x == 100);
    CHECK(r.current() == c.current());
    const Id fresh = r.addScene("New");
    for (auto& s : c.scenes()) CHECK(s.id != fresh);   // ids aren't reused

    // Only the last scene can't go; removing a source removes its items.
    r.removeSource(cam);
    CHECK(r.scene(gaming)->items.empty());
    while (r.scenes().size() > 1) r.removeScene(r.scenes().back().id);
    CHECK(!r.removeScene(r.scenes()[0].id));

    // Not a collection: left as it was.
    SceneCollection bad;
    CHECK(!bad.fromJson("{\"nope\":1}") && bad.scenes().size() == 1);
    CHECK(!bad.fromJson("not json"));
    // Kinds this version doesn't know are left out, not invented.
    CHECK(bad.fromJson(R"({"scenes":[{"id":"c1","name":"A","items":[{"source":"s1"},{"source":"s2"}]}],
                          "sources":[{"id":"s1","kind":"hologram"},{"id":"s2","kind":"image","name":"Logo"}]})"));
    CHECK(bad.sources().size() == 1 && bad.scenes()[0].items.size() == 1);
    CHECK(bad.addSource(SourceKind::Text, "x").substr(1) != "2");   // past the ids in the file
    CHECK(kindFromKey("app-audio") == SourceKind::ApplicationAudio && isAudioOnly(SourceKind::DesktopAudio) && !isAudioOnly(SourceKind::Window));
}

static void testRouting() {
    std::printf("routing\n");
    using namespace ncap::routing;
    AudioSource mic{"m", "Microphone", Kind::Input, "USB Mic", "", {}, Stream | Track1, false, 1.0f};
    AudioSource game{"g", "Minecraft", Kind::Application, "", "Minecraft.exe", {}, Stream | Track1 | Monitor, false, 1.0f};
    AudioSource spotify{"s", "Spotify", Kind::Application, "", "spotify.exe", {}, Monitor, false, 1.0f};
    AudioSource discord{"d", "Discord", Kind::Application, "", "discord.exe", {}, Monitor, false, 1.0f};
    const std::vector<PlayingApp> playing = {{"minecraft.exe", ""}, {"Spotify.exe", ""}, {"discord.exe", ""}, {"shellexperiencehost.exe", ""}};

    // The brief's case: Minecraft and the mic on stream; Spotify and Discord private. No warnings.
    auto w = analyse({mic, game, spotify, discord}, playing, "", true);
    CHECK(w.empty());

    // Adding whole desktop audio to the stream leaks Spotify and Discord, and doubles Minecraft.
    AudioSource desk{"k", "Desktop audio", Kind::Desktop, "", "", {}, Stream | Track1, false, 1.0f};
    w = analyse({mic, game, spotify, discord, desk}, playing, "", true);
    int leaks = 0, twice = 0;
    for (auto& x : w) {
        if (x.type == Warning::LeakThroughDesktop) ++leaks;
        if (x.type == Warning::CapturedTwice && x.app == "minecraft.exe") ++twice;
    }
    CHECK(leaks == 2 && twice == 1);
    CHECK(w[0].message.find("viewers will still hear it") != std::string::npos);

    // Desktop audio "except Spotify and Discord" (and except Minecraft, which has its own source): clean.
    desk.excluded = {"SPOTIFY.EXE", "discord.exe", "minecraft.exe"};
    CHECK(analyse({mic, game, spotify, discord, desk}, playing, "", true).empty());

    // A leak only counts on the device the desktop source captures.
    desk.excluded = {};
    desk.device = "Speakers";
    w = analyse({mic, game, spotify, discord, desk}, playing, "", true);
    CHECK(w.empty());

    // Muted or silent desktop audio doesn't leak.
    desk.device = "";
    desk.muted = true;
    CHECK(analyse({mic, game, spotify, discord, desk}, playing, "", true).empty());

    // Monitoring desktop audio on its own device feeds back.
    AudioSource deskMon{"k2", "Desktop audio", Kind::Desktop, "", "", {"spotify.exe", "discord.exe", "minecraft.exe"}, Monitor, false, 1.0f};
    w = analyse({deskMon}, playing, "", true);
    CHECK(w.size() == 1 && w[0].type == Warning::MonitorFeedback);
    CHECK(analyse({deskMon}, playing, "Headphones", true).empty());

    // Speakers for monitoring with a live mic: echo.
    w = analyse({mic, game}, playing, "Speakers", false);
    CHECK(w.size() == 1 && w[0].type == Warning::EchoOnMicrophone);

    // Two sources for one app.
    AudioSource game2 = game;
    game2.id = "g2";
    w = analyse({game, game2}, playing, "", true);
    CHECK(w.size() == 1 && w[0].type == Warning::CapturedTwice && w[0].sources.size() == 2);

    CHECK(exeKey("C:\\Games\\Minecraft\\Minecraft.EXE") == "minecraft.exe");
    CHECK(describeRoute(game) == "Minecraft (application) → Stream · Track 1 · Monitor");
    CHECK(describeRoute(spotify) == "Spotify (application) → Monitor");
    AudioSource off = spotify;
    off.muted = true;
    CHECK(describeRoute(off) == "Spotify (application) → muted");
    CHECK(describeRoute(desk).find("Everything playing on the default output") == 0);
}

static void testDiagnostics() {
    std::printf("diagnostics\n");
    const std::string bullet = "\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2";
    CHECK(redact("connecting to rtmp://live.twitch.tv/app/live_123456789_AbCdEfGhIjKlMnOp now") ==
          "connecting to rtmp://live.twitch.tv/app/" + bullet + " now");
    CHECK(redact("rtmps://a.rtmps.youtube.com:443/live2/abcd-efgh-ijkl-mnop-qrst") == "rtmps://a.rtmps.youtube.com:443/live2/" + bullet);
    CHECK(redact("rtmp://example.com/live") == "rtmp://example.com/live");   // no key in it
    CHECK(redact("GET /x?user=cj&token=abc123&n=1") == "GET /x?user=cj&token=" + bullet + "&n=1");
    CHECK(redact("password=hunter2, ok") == "password=" + bullet + ", ok");
    CHECK(redact("Authorization: Bearer eyJhbGciOi.abc") == "Authorization: Bearer " + bullet);
    CHECK(redact("key sk_live_0123456789abcdef in text") == "key " + bullet + " in text");
    CHECK(redact("the key is MySecretKey99 ok", {"MySecretKey99"}) == "the key is " + bullet + " ok");
    CHECK(redact("monkey=banana") == "monkey=banana");   // only whole names
    CHECK(redact("nothing secret here") == "nothing secret here");

    Layout l;
    CHECK(l.order.size() == 4 && l.shows(Layout::Mixer));
    l.hide(Layout::Sources);
    CHECK(!l.shows(Layout::Sources) && l.order.size() == 3);
    float sum = 0;
    for (int p : l.order) sum += l.widths[(size_t)p];
    CHECK(sum > 0.999f && sum < 1.001f);
    l.show(Layout::Sources);
    CHECK(l.order.back() == Layout::Sources);
    l.move(Layout::Sources, -1);
    CHECK(l.order[2] == Layout::Sources);
    const float before = l.widths[(size_t)l.order[0]];
    l.resizeDivider(0, 0.05f);
    CHECK(l.widths[(size_t)l.order[0]] > before);
    l.resizeDivider(0, 5.0f);   // never squeezes the next panel away
    CHECK(l.widths[(size_t)l.order[1]] >= 0.08f - 1e-6f);
    const Layout r = Layout::fromJson(l.toJson());
    CHECK(r.order == l.order && r.dockHeight == l.dockHeight);
    const Layout junk = Layout::fromJson(R"({"order":[9,9,1,1],"widths":[1],"dockHeight":5})");
    CHECK(junk.order == std::vector<int>{1} && junk.dockHeight <= 0.8f && junk.widths.size() == 4);
    Layout one;
    one.order = {Layout::Scenes};
    one.hide(Layout::Scenes);   // the last panel stays
    CHECK(one.order.size() == 1);
}

int main() {
    testScenes();
    testItemEditing();
    testPlacement();
    testAudio();
    testRouting();
    testDiagnostics();
    std::printf("\n%d passed, %d failed\n", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

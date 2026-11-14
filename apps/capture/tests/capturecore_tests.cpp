// Unit tests for NorthRay Capture's portable core. Dependency-free like the other suites.
#include "ncap/diag.hpp"
#include "ncap/routing.hpp"
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
    testRouting();
    testDiagnostics();
    std::printf("\n%d passed, %d failed\n", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

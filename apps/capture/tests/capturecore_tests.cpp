// Unit tests for NorthRay Capture's portable core. Dependency-free like the other suites.
#include "ncap/audio.hpp"
#include "ncap/diag.hpp"
#include "ncap/media.hpp"
#include "ncap/mkv.hpp"
#include "ncap/oauth.hpp"
#include "ncap/transition.hpp"
#include "ncap/sourceutil.hpp"
#include "ncap/rtmp.hpp"
#include "ncap/placement.hpp"
#include "ncap/routing.hpp"

#include <cmath>
#include "ncap/scenes.hpp"

#include <cstdio>
#include <string>

using namespace ncap;
namespace media = ncap::media;

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

static void testMedia() {
    std::printf("media\n");
    using namespace ncap::media;
    // An access unit: AUD, SPS, PPS, IDR slice; mixed 3- and 4-byte start codes.
    const Bytes au = {0, 0, 0, 1, 0x09, 0xF0, 0, 0, 0, 1, 0x67, 0x64, 0x00, 0x28, 0xAC, 0, 0, 1, 0x68, 0xEE, 0x3C, 0x80, 0, 0, 1, 0x65, 0x88, 0x84, 0x00};
    const auto nals = nalUnits(au.data(), au.size());
    CHECK(nals.size() == 4 && nalType(nals[1].first) == 7 && nals[1].second == 5 && nalType(nals[3].first) == 5 && nals[3].second == 4);
    CHECK(containsKeyframe(au.data(), au.size()));
    Bytes sps, pps;
    CHECK(findParameterSets(au.data(), au.size(), sps, pps) && sps.size() == 5 && pps.size() == 4);
    const Bytes lp = annexBToLengthPrefixed(au.data(), au.size(), true);   // only the slice is left
    CHECK(lp.size() == 8 && lp[3] == 4 && lp[4] == 0x65);
    CHECK(annexBToLengthPrefixed(au.data(), au.size(), false).size() == 4 * 4 + 2 + 5 + 4 + 4);
    const Bytes c = avcC(sps, pps);
    CHECK(c.size() == 6 + 2 + 5 + 1 + 2 + 4 && c[0] == 1 && c[1] == 0x64 && c[3] == 0x28 && c[4] == 0xFF && c[5] == 0xE1 && c[7] == 5);
    const Bytes asc = aacConfig(48000, 2);
    CHECK(asc.size() == 2 && asc[0] == 0x11 && asc[1] == 0x90 && aacConfig(44100, 2)[1] == 0x10 && aacConfig(12345, 2).empty());
    CHECK(recordingName(2026, 11, 4, 9, 5, 3) == "2026-11-04 09-05-03");

    // Interleaving: video every 16.7 ms, audio every 21.3 ms; out in time order, and nothing waits
    // on a stream that has gone quiet for longer than the limit.
    Interleaver il(2, 100000);
    auto pkt = [](int s, std::int64_t t) {
        Packet p;
        p.stream = s;
        p.pts = p.dts = t;
        return p;
    };
    il.push(pkt(0, 0));
    il.push(pkt(0, 16667));
    Packet out;
    CHECK(!il.pop(out));   // audio may still bring something earlier
    il.push(pkt(1, 0));
    il.push(pkt(1, 21333));
    std::vector<std::int64_t> order;
    while (il.pop(out)) order.push_back(out.dts * 10 + out.stream);
    CHECK(order.size() == 3 && order[0] == 0 && order[1] == 1 && order[2] == 166670);   // audio 21333 waits for video's next
    for (int i = 2; i < 12; ++i) il.push(pkt(0, i * 16667));   // audio stops: video carries on after 100 ms
    int released = 0;
    while (il.pop(out)) ++released;
    CHECK(released == 11 && il.queued() == 0);   // the quiet audio holds nothing up
    Interleaver fresh(2, 100000);                  // a stream that hasn't started yet is waited for, up to the limit
    fresh.push(pkt(0, 0));
    CHECK(!fresh.pop(out));
    fresh.push(pkt(0, 150000));
    CHECK(fresh.pop(out) && out.dts == 0);
    CHECK(fresh.drain(out) && out.dts == 150000 && !fresh.drain(out));

    // The Matroska writer: header, blocks, and the sizes and duration filled in at the end.
    ncap::mkv::MemorySink sink;
    ncap::mkv::Writer w;
    ncap::mkv::VideoTrack v;
    v.width = 1280;
    v.height = 720;
    v.fps = 30;
    v.avcC = c;
    CHECK(w.begin(&sink, v, {{"Track 1", 48000, 2, asc}, {"Microphone", 48000, 2, asc}}, "NorthRay Capture test"));
    CHECK(sink.bytes.size() > 40 && sink.bytes[0] == 0x1A && sink.bytes[1] == 0x45 && sink.bytes[2] == 0xDF && sink.bytes[3] == 0xA3);
    CHECK(w.video(0, true, lp) && w.audio(0, 0, {1, 2, 3}) && w.audio(1, 10000, {4, 5}) && w.video(33333, false, lp) && w.video(1000000, true, lp));
    const std::size_t before = sink.bytes.size();
    CHECK(w.finish() && sink.bytes.size() > before && w.durationUs() == 1000000);
    // The segment's size is no longer "unknown".
    std::size_t seg = 0;
    for (std::size_t i = 0; i + 4 < sink.bytes.size(); ++i)
        if (sink.bytes[i] == 0x18 && sink.bytes[i + 1] == 0x53 && sink.bytes[i + 2] == 0x80 && sink.bytes[i + 3] == 0x67) {
            seg = i + 4;
            break;
        }
    CHECK(seg > 0 && sink.bytes[seg] == 0x01 && sink.bytes[seg + 7] != 0xFF);
}

static void testRtmp() {
    std::printf("rtmp\n");
    using namespace ncap::rtmp;
    Url u;
    std::string key;
    CHECK(parseUrl("rtmps://ingest.global-contribute.live-video.net/app", u, &key) && u.tls && u.port == 443 && u.app == "app" &&
          u.host == "ingest.global-contribute.live-video.net" && u.tcUrl == "rtmps://ingest.global-contribute.live-video.net/app" && key.empty());
    CHECK(parseUrl(" rtmp://a.rtmp.youtube.com/live2/ ", u) && !u.tls && u.port == 1935 && u.app == "live2" && u.tcUrl == "rtmp://a.rtmp.youtube.com/live2");
    CHECK(parseUrl("rtmps://a.rtmps.youtube.com:443/live2", u) && u.port == 443 && u.tcUrl == "rtmps://a.rtmps.youtube.com/live2");
    CHECK(parseUrl("RTMP://127.0.0.1:19350/live/abc-123", u, &key) && u.port == 19350 && u.app == "live" && key == "abc-123" &&
          u.tcUrl == "rtmp://127.0.0.1:19350/live");
    CHECK(!parseUrl("https://twitch.tv", u) && !parseUrl("rtmp://", u) && !parseUrl("rtmp://host", u) && !parseUrl("rtmp://host:99999/app", u) &&
          !parseUrl("rtmp://host:x/app", u));

    // AMF0 round trip.
    media::Bytes b;
    amfWrite(b, Amf::str("connect"));
    amfWrite(b, Amf::num(1));
    amfWrite(b, Amf::object({{"app", Amf::str("live2")}, {"fpad", Amf::boolean_(false)}, {"n", Amf::null()}}));
    amfWrite(b, Amf::array({{"width", Amf::num(1920)}}));
    std::size_t pos = 0;
    Amf a, c, o, arr;
    CHECK(amfRead(b, pos, a) && a.string == "connect" && amfRead(b, pos, c) && c.number == 1);
    CHECK(amfRead(b, pos, o) && o.type == Amf::Object && o.get("app") && o.get("app")->string == "live2" && o.get("fpad")->type == Amf::Boolean &&
          o.get("n")->type == Amf::Null && !o.get("missing"));
    CHECK(amfRead(b, pos, arr) && arr.type == Amf::EcmaArray && arr.get("width")->number == 1920 && pos == b.size());
    media::Bytes cut(b.begin(), b.begin() + 5);
    pos = 0;
    CHECK(!amfRead(cut, pos, a));   // cut short: refused, not read past the end

    // FLV bodies.
    const auto v = flvVideo(true, false, {9, 9});
    CHECK(v.size() == 7 && v[0] == 0x17 && v[1] == 1 && v[5] == 9);
    CHECK(flvVideo(false, true, {})[0] == 0x27 && flvVideo(false, true, {})[1] == 0);
    const auto au = flvAudio(true, {0x11, 0x90});
    CHECK(au.size() == 4 && au[0] == 0xAF && au[1] == 0 && au[2] == 0x11);
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
    // Defaults and labels; what a desktop source carries.
    CHECK(defaultBuses(Kind::Input) == (Stream | Track1 | Track2) && (defaultBuses(Kind::Application) & Monitor) == 0);
    CHECK(busLabel(Stream) == "Stream" && busLabel(Track5) == "Track 5" && busLabel(Monitor) == "Monitor");
    AudioSource everything{"e", "Desktop audio", Kind::Desktop, "", "", {"Spotify.exe"}, Stream, false, 1.0f};
    const auto carried = carriedApps(everything, {{"Minecraft.exe", ""}, {"spotify.exe", ""}, {"chrome.exe", "Headphones"}, {"discord.exe", ""}});
    CHECK(carried.size() == 2 && carried[0] == "discord.exe" && carried[1] == "minecraft.exe");
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

static std::string hexOf(const std::array<std::uint8_t, 32>& d) {
    static const char* h = "0123456789abcdef";
    std::string s;
    for (auto b : d) { s += h[b >> 4]; s += h[b & 15]; }
    return s;
}

static void testOAuth() {
    std::printf("oauth\n");
    namespace o = ncap::oauth;
    CHECK(hexOf(o::sha256("", 0)) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(hexOf(o::sha256("abc", 3)) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    const std::string l = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";   // 56 bytes: needs a second block
    CHECK(hexOf(o::sha256(l.data(), l.size())) == "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    std::string m(1000, 'a');
    CHECK(hexOf(o::sha256(m.data(), m.size())) == "41edece42d63e8d9bf515a9ba6932e1c20cbc9f5a5d134645adb5db1b9737ea3");
    // RFC 7636 appendix B.
    const std::uint8_t r[32] = {116, 24, 223, 180, 151, 153, 224, 37, 79, 250, 96, 125, 216, 173, 187, 186,
                                22, 212, 37, 77, 105, 214, 191, 240, 91, 88, 5, 88, 83, 132, 141, 121};
    CHECK(o::pkceVerifier(r) == "dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk");
    CHECK(o::pkceChallenge("dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk") == "E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM");
    const std::uint8_t b[] = {0xfb, 0xff};
    CHECK(o::base64url(b, 2) == "-_8" && o::base64url(b, 1) == "-w");
    CHECK(o::urlEncode("a b&c=d/é~") == "a%20b%26c%3Dd%2F%C3%A9~");
    CHECK(o::urlDecode("a+b%26c%3d") == "a b&c=");
    CHECK(o::form({{"scope", "channel:read:stream_key"}, {"x", "1"}}) == "scope=channel%3Aread%3Astream_key&x=1");
    auto q = o::parseQuery("code=4%2F0Ab&state=xyz&empty&scope=a+b");
    CHECK(q["code"] == "4/0Ab" && q["state"] == "xyz" && q.count("empty") && q["scope"] == "a b");

    auto c = o::parseClients(R"({"twitch":{"clientId":"abc"},"google":{"clientId":"g.apps","clientSecret":"s"}})");
    CHECK(c.twitch() && c.google() && c.twitchClientId == "abc" && c.googleClientSecret == "s");
    CHECK(!o::parseClients("not json").twitch() && !o::parseClients("{}").google());

    // Twitch device flow.
    auto d = o::parseDeviceCode(200, R"({"device_code":"dc","expires_in":1800,"interval":5,"user_code":"ABCDEFGH","verification_uri":"https://www.twitch.tv/activate?public=true&device-code=ABCDEFGH"})");
    CHECK(d.error.empty() && d.userCode == "ABCDEFGH" && d.interval == 5);
    CHECK(!o::parseDeviceCode(200, R"({"device_code":"dc","verification_uri":"https://evil.example/activate"})").error.empty());
    CHECK(!o::parseDeviceCode(400, R"({"status":400,"message":"invalid client"})").error.empty());
    auto pending = o::parseTokens(400, R"({"status":400,"message":"authorization_pending"})");
    CHECK(!pending.ok() && o::classifyPoll(pending) == o::Poll::Pending);
    CHECK(o::classifyPoll(o::parseTokens(400, R"({"status":400,"message":"invalid device code"})")) == o::Poll::Failed);
    auto tok = o::parseTokens(200, R"({"access_token":"at","expires_in":14400,"refresh_token":"rt","scope":["channel:read:stream_key"],"token_type":"bearer"})");
    CHECK(tok.ok() && tok.refresh == "rt" && tok.expiresIn == 14400 && o::classifyPoll(tok) == o::Poll::Done);
    CHECK(o::classifyPoll(o::parseTokens(0, "")) == o::Poll::Pending);   // a dropped connection: keep polling
    auto g = o::parseTokens(400, R"({"error":"invalid_grant","error_description":"Token has been expired or revoked."})");
    CHECK(!g.ok() && g.error == "invalid_grant");
    auto u = o::parseValidate(200, R"({"client_id":"abc","login":"cjphilip","scopes":["channel:read:stream_key"],"user_id":"141981764","expires_in":5520838})");
    CHECK(u.valid && u.login == "cjphilip" && u.userId == "141981764");
    CHECK(!o::parseValidate(401, R"({"status":401,"message":"invalid access token"})").valid);
    std::string msg;
    CHECK(o::parseTwitchKey(200, R"({"data":[{"stream_key":"live_44322889_a34ub37c8ajv98a0"}]})", msg) == "live_44322889_a34ub37c8ajv98a0");
    CHECK(o::parseTwitchKey(403, R"({"error":"Forbidden","status":403,"message":"Phone verification is required"})", msg).empty() &&
          msg == "Twitch: Phone verification is required");
    CHECK(o::parseTwitchKey(401, R"({"error":"Unauthorized","status":401,"message":"Invalid OAuth token"})", msg).empty() &&
          msg.find("Sign in again") != std::string::npos);

    // YouTube streams: the default first, non-RTMP ones left out.
    auto ys = o::parseYouTubeStreams(200, R"({"items":[
        {"snippet":{"title":"HLS"},"cdn":{"ingestionType":"hls","ingestionInfo":{"streamName":"h","ingestionAddress":"https://x"}}},
        {"snippet":{"title":"Other","isDefaultStream":false},"cdn":{"ingestionType":"rtmp","ingestionInfo":{"streamName":"k2","ingestionAddress":"rtmp://a.rtmp.youtube.com/live2","rtmpsIngestionAddress":"rtmps://a.rtmps.youtube.com/live2"}},"contentDetails":{"isReusable":true}},
        {"snippet":{"title":"Default stream key","isDefaultStream":true},"cdn":{"ingestionType":"rtmp","ingestionInfo":{"streamName":"k1","ingestionAddress":"rtmp://a.rtmp.youtube.com/live2","rtmpsIngestionAddress":"rtmps://a.rtmps.youtube.com/live2"}},"contentDetails":{"isReusable":true}}]})", msg);
    CHECK(ys.size() == 2 && ys[0].key == "k1" && ys[0].isDefault && ys[0].rtmps == "rtmps://a.rtmps.youtube.com/live2" && ys[1].key == "k2");
    CHECK(o::parseYouTubeStreams(200, R"({"items":[]})", msg).empty());
    CHECK(o::parseYouTubeStreams(403, R"({"error":{"code":403,"message":"The user is not enabled for live streaming."}})", msg).empty() &&
          msg.find("not enabled for live streaming") != std::string::npos);

    CHECK(o::parseYouTubeChannel(200, R"({"items":[{"id":"UC1","snippet":{"title":"CJ Philip"}}]})") == "CJ Philip");
    CHECK(o::parseYouTubeChannel(200, R"({"items":[]})").empty());
    // Google's sign-in and the loopback redirect.
    o::Endpoints e;
    const std::string url = o::googleAuthUrl(e, c, "http://127.0.0.1:50123/", "st8", "chal");
    CHECK(url.rfind("https://accounts.google.com/o/oauth2/v2/auth?client_id=g.apps&redirect_uri=http%3A%2F%2F127.0.0.1%3A50123%2F", 0) == 0);
    CHECK(url.find("code_challenge_method=S256") != std::string::npos && url.find("youtube.readonly") != std::string::npos);
    auto rd = o::parseRedirect("GET /?state=st8&code=4%2F0AbC&scope=x HTTP/1.1", "st8");
    CHECK(rd.isCallback && rd.stateOk && rd.code == "4/0AbC");
    rd = o::parseRedirect("GET /?state=other&code=4%2F0AbC HTTP/1.1", "st8");
    CHECK(rd.isCallback && !rd.stateOk && rd.code.empty() && rd.error == "state_mismatch");
    rd = o::parseRedirect("GET /?error=access_denied&state=st8 HTTP/1.1", "st8");
    CHECK(rd.code.empty() && rd.error == "access_denied");
    CHECK(!o::parseRedirect("GET /favicon.ico HTTP/1.1", "st8").isCallback);
    CHECK(!o::parseRedirect("POST /?code=a&state=st8 HTTP/1.1", "st8").isCallback);
}

static void testTransitions() {
    std::printf("transitions\n");
    namespace t = ncap::transition;
    auto near = [](float a, float b) { return std::fabs(a - b) < 1e-3f; };
    for (auto e : {t::Easing::Linear, t::Easing::EaseInOut, t::Easing::EaseOut}) {
        CHECK(near(t::ease(e, 0), 0) && near(t::ease(e, 1), 1) && near(t::ease(e, -1), 0) && near(t::ease(e, 2), 1));
        float prev = 0;
        bool rising = true;
        for (int i = 1; i <= 100; ++i) {
            const float v = t::ease(e, i / 100.0f);
            rising = rising && v >= prev - 1e-6f;
            prev = v;
        }
        CHECK(rising);
    }
    CHECK(near(t::ease(t::Easing::EaseInOut, 0.5f), 0.5f) && t::ease(t::Easing::EaseOut, 0.5f) > 0.5f);
    // Slide left: the new scene comes from the right, the old one leaves to the left.
    auto o = t::offsets(t::Kind::Slide, t::Direction::Left, 0, 1920, 1080);
    CHECK(near(o.toX, 1920) && near(o.fromX, 0) && near(o.toY, 0));
    o = t::offsets(t::Kind::Slide, t::Direction::Left, 0.5f, 1920, 1080);
    CHECK(near(o.toX, 960) && near(o.fromX, -960));
    o = t::offsets(t::Kind::Slide, t::Direction::Left, 1, 1920, 1080);
    CHECK(near(o.toX, 0) && near(o.fromX, -1920));
    o = t::offsets(t::Kind::Swipe, t::Direction::Down, 0.25f, 1920, 1080);   // swipe: the old one stays put
    CHECK(near(o.toY, -810) && near(o.fromY, 0) && near(o.fromX, 0));
    o = t::offsets(t::Kind::Slide, t::Direction::Up, 0.5f, 1920, 1080);
    CHECK(near(o.toY, 540) && near(o.fromY, -540));
    // Luma patterns: the right size, the ends where they should be.
    auto h = t::pattern(t::Pattern::Horizontal, 64, 36);
    CHECK(h.size() == 64u * 36u && h[0] == 0 && h[63] == 255 && h[35 * 64] == 0);
    auto c = t::pattern(t::Pattern::Circle, 65, 37);
    CHECK(c[18 * 65 + 32] == 0 && c[0] == 255);
    auto k = t::pattern(t::Pattern::Clock, 65, 65);
    CHECK(k[0 * 65 + 33] < 8 && k[32 * 65 + 64] > 55 && k[32 * 65 + 64] < 72 && k[64 * 65 + 32] > 120 && k[64 * 65 + 32] < 136);
    auto b = t::pattern(t::Pattern::Blinds, 81, 2);
    CHECK(b[0] == 0 && b[10] == 0 && b[9] > 200);
    CHECK(t::pattern(t::Pattern::Diagonal, 0, 10).empty());
    // The luma wipe shows nothing at 0 and everything at 1, whatever the softness.
    for (float soft : {0.0f, 0.1f, 0.5f})
        for (float l : {0.0f, 0.3f, 1.0f}) {
            CHECK(near(t::lumaAlpha(l, 0, soft, false), 0));
            CHECK(near(t::lumaAlpha(l, 1, soft, false), 1));
        }
    CHECK(t::lumaAlpha(0.1f, 0.5f, 0.1f, false) > 0.99f && t::lumaAlpha(0.9f, 0.5f, 0.1f, false) < 0.01f);
    CHECK(t::lumaAlpha(0.9f, 0.5f, 0.1f, true) > 0.99f);   // inverted: the far end first
    // Settings round trip; junk keeps the defaults.
    t::Settings s;
    s.kind = t::Kind::LumaWipe;
    s.durationMs = 750;
    s.pattern = t::Pattern::Clock;
    s.direction = t::Direction::Up;
    s.invert = true;
    const t::Settings r = t::fromJson(t::toJson(s));
    CHECK(r.kind == t::Kind::LumaWipe && r.durationMs == 750 && r.pattern == t::Pattern::Clock && r.direction == t::Direction::Up && r.invert);
    const t::Settings bad = t::fromJson(R"({"kind":"explode","durationMs":999999,"softness":3})");
    CHECK(bad.kind == t::Kind::Fade && bad.durationMs == 10000 && near(bad.softness, 0.5f));
}

static void testNesting() {
    std::printf("nesting\n");
    SceneCollection c;
    const Id a = c.current(), b = c.addScene("Camera"), d = c.addScene("Overlay");
    const Id sb = c.sceneSource(b);
    CHECK(!sb.empty() && c.sceneSource(b) == sb && c.source(sb)->kind == SourceKind::Scene && SceneCollection::nestedScene(*c.source(sb)) == b);
    CHECK(c.sceneSource("nope").empty());
    CHECK(!c.addItem(a, sb).empty());                       // Camera inside Scene
    const Id sa = c.sceneSource(a);
    CHECK(c.addItem(b, sa).empty());                        // Scene inside Camera would loop
    CHECK(c.addItem(a, sa).empty());                        // a scene inside itself
    const Id sd = c.sceneSource(d);
    CHECK(!c.addItem(b, sd).empty());                       // Overlay inside Camera
    CHECK(c.wouldLoop(d, a) && !c.wouldLoop(a, d));         // Scene shows Camera shows Overlay
    CHECK(c.addItem(d, sa).empty());
    CHECK(c.unusedSources().size() == 1 && c.unusedSources()[0] == sa);   // Scene's own source isn't shown anywhere
    // Removing a scene removes the source that showed it, and its items elsewhere.
    CHECK(c.removeScene(b));
    CHECK(!c.source(sb) && c.scene(a)->items.empty());
    // Duplicating an item: its own transform, the same source.
    const Id img = c.addSource(SourceKind::Colour, "Red");
    Transform t;
    t.x = 100;
    const Id i1 = c.addItem(a, img, t);
    const Id i2 = c.duplicateItem(a, i1);
    CHECK(!i2.empty() && i2 != i1 && c.item(a, i2)->source == img && c.item(a, i2)->transform.x == 124);
    CHECK(c.scene(a)->items.size() == 2 && c.scene(a)->items[1].id == i2);
    // Survives a save.
    const Id sd2 = c.sceneSource(d);
    c.addItem(a, sd2);
    SceneCollection r;
    CHECK(r.fromJson(c.toJson()) && r.wouldLoop(d, a) && SceneCollection::nestedScene(*r.source(sd2)) == d);
}

static void testSourceUtil() {
    std::printf("source helpers\n");
    namespace su = ncap::sourceutil;
    su::Rgba c;
    CHECK(su::parseColour("#FF8000", c) && c.r == 255 && c.g == 128 && c.b == 0 && c.a == 255);
    CHECK(su::parseColour(" 00ff0080 ", c) && c.g == 255 && c.a == 128);
    CHECK(su::parseColour("#abc", c) && c.r == 0xAA && c.b == 0xCC);
    CHECK(!su::parseColour("#12345", c) && !su::parseColour("red", c) && !su::parseColour("", c));
    CHECK(su::colourText({255, 128, 0, 255}) == "#FF8000" && su::colourText({0, 0, 0, 128}) == "#00000080");
    su::Placement p;
    CHECK(su::parsePlacement("120, 80, 640, 360", p) && p.hasX && p.hasY && p.hasSize && p.x == 120 && p.h == 360);
    CHECK(su::parsePlacement("10 -20.5", p) && !p.hasSize && p.y == -20.5f);
    CHECK(su::parsePlacement("- 300", p) && !p.hasX && p.hasY && p.y == 300);
    CHECK(!su::parsePlacement("1, 2, 3", p) && !su::parsePlacement("1, 2, 0, 5", p) && !su::parsePlacement("a, b", p) && !su::parsePlacement("5", p));
    Transform t;
    t.x = 12.34f;
    t.y = 5;
    CHECK(su::placementText(t, 1920, 1080) == "12.3, 5, 1920, 1080");
    t.width = 640;
    t.height = 360;
    CHECK(su::placementText(t, 1920, 1080) == "12.3, 5, 640, 360");
    float d = 0;
    CHECK(su::parseDegrees("45", d) && d == 45);
    CHECK(su::parseDegrees("270\xC2\xB0", d) && d == -90);
    CHECK(su::parseDegrees("-540", d) && d == 180);
    CHECK(!su::parseDegrees("north", d));
    auto o = su::slideOrder({"img10.png", "img2.png", "Img1.png", "b.jpg", "a.jpg"}, false, 0);
    CHECK(o.size() == 5 && o[0] == "a.jpg" && o[1] == "b.jpg" && o[2] == "Img1.png" && o[3] == "img2.png" && o[4] == "img10.png");
    auto s1 = su::slideOrder({"1", "2", "3", "4", "5", "6"}, true, 42), s2 = su::slideOrder({"6", "5", "4", "3", "2", "1"}, true, 42);
    CHECK(s1 == s2 && s1 != su::slideOrder({"1", "2", "3", "4", "5", "6"}, false, 0));
    CHECK(su::isImageFile("x.PNG") && su::isImageFile("a.b.jpeg") && !su::isImageFile("notes.txt") && !su::isImageFile("png"));
    CHECK(su::gifDelayMs(0) == 100 && su::gifDelayMs(1) == 100 && su::gifDelayMs(4) == 40);
    CHECK(kindFromKey("slideshow") == SourceKind::Slideshow && std::string(kindKey(SourceKind::Slideshow)) == "slideshow");
}

int main() {
    testScenes();
    testItemEditing();
    testPlacement();
    testAudio();
    testMedia();
    testRtmp();
    testRouting();
    testDiagnostics();
    testOAuth();
    testTransitions();
    testNesting();
    testSourceUtil();
    std::printf("\n%d passed, %d failed\n", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

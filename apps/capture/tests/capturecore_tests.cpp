// Unit tests for NorthRay Capture's portable core. Dependency-free like the other suites.
#include "ncap/audio.hpp"
#include "ncap/diag.hpp"
#include "ncap/media.hpp"
#include "ncap/mkv.hpp"
#include "ncap/oauth.hpp"
#include "ncap/transition.hpp"
#include "ncap/sourceutil.hpp"
#include "ncap/dsp.hpp"
#include "ncap/border.hpp"
#include "ncap/chat.hpp"
#include "ncap/mp4.hpp"
#include "ncap/profiles.hpp"
#include "ncap/wizard.hpp"
#include "ncap/soundboard.hpp"
#include "ncap/keying.hpp"
#include "ncap/design.hpp"
#include "ncap/plugins.hpp"
#include "ncap/rtmp.hpp"
#include "ncap/placement.hpp"
#include "ncap/automation.hpp"
#include "ncap/checklist.hpp"
#include "ncap/gif.hpp"
#include "ncap/routing.hpp"

#include <cmath>
#include "ncap/scenes.hpp"

#include <array>
#include <cstdio>
#include <cstring>
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

    // The second output: desktop audio on the Stream bus played onto the device it captures feeds back;
    // onto another device it doesn't; sharing the monitor's device is pointed out.
    AudioSource deskStream{"k3", "Desktop audio", Kind::Desktop, "Speakers", "", {"spotify.exe", "discord.exe", "minecraft.exe"}, Stream, false, 1.0f};
    w = analyse({deskStream}, playing, "Headphones", true, "speakers", Stream);
    CHECK(w.size() == 1 && w[0].type == Warning::OutputFeedback);
    CHECK(analyse({deskStream}, playing, "Headphones", true, "Line out", Stream).empty());
    CHECK(analyse({deskStream}, playing, "Headphones", true, "Speakers", Track2).empty());   // it isn't on Track 2
    w = analyse({mic, game}, playing, "Headphones", true, "headphones", Stream);
    CHECK(w.size() == 1 && w[0].type == Warning::OutputsShareDevice);
    CHECK(analyse({mic, game}, playing, "Headphones", true).empty());   // second output off

    // Presets.
    CHECK(applyPreset(Preset::StreamAndRecord, Kind::Input, Monitor) == (Stream | Track1 | Track2 | Monitor));
    CHECK(applyPreset(Preset::RecordOnly, Kind::Desktop, Stream | Track1 | Track3) == (Track1 | Track3));
    CHECK(applyPreset(Preset::StreamOnly, Kind::Application, Stream | Track1 | Track4) == Stream);
    CHECK(applyPreset(Preset::MonitorOnly, Kind::Application, Stream | Track1) == Monitor);
    Preset pr;
    CHECK(matchPreset(defaultBuses(Kind::Input), Kind::Input, pr) && pr == Preset::StreamAndRecord);
    CHECK(matchPreset(Track1 | Track4 | Monitor, Kind::Application, pr) && pr == Preset::RecordOnly);
    CHECK(!matchPreset(Stream | Track5, Kind::Application, pr));

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
    CHECK(junk.order == std::vector<int>{1} && junk.dockHeight <= 0.8f && junk.widths.size() == (size_t)Layout::kPanels);
    // A layout saved before the Chat panel: its widths stay, Chat is hidden until shown.
    const Layout old = Layout::fromJson(R"({"order":[0,1,2,3],"widths":[0.1,0.4,0.3,0.2]})");
    CHECK(!old.shows(Layout::Chat) && old.widths.size() == (size_t)Layout::kPanels && old.widths[1] > 0.39f && old.widths[1] < 0.41f);
    Layout withChat = old;
    withChat.show(Layout::Chat);
    CHECK(withChat.shows(Layout::Chat) && withChat.order.back() == Layout::Chat && std::string(Layout::panelName(Layout::Chat)) == "Chat");
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
    t::Settings st;
    st.kind = t::Kind::Stinger;
    st.stingerFile = "C:\\stinger.mp4";
    st.cutMs = 700;
    const t::Settings st2 = t::fromJson(t::toJson(st));
    CHECK(st2.kind == t::Kind::Stinger && st2.stingerFile == "C:\\stinger.mp4" && st2.cutMs == 700);
    CHECK(t::shaderSource("float4 transition(float2 uv, float p) { return 1; }").find("float4 ps(") != std::string::npos);
    CHECK(t::exampleShader().find("float4 transition(float2 uv, float progress)") != std::string::npos);
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
    CHECK(su::isMediaFile("clip.MP4") && su::isMediaFile("song.flac") && !su::isMediaFile("a.png") && !su::isMediaFile("mp4"));
    CHECK(su::clockText(65) == "1:05" && su::clockText(3723) == "1:02:03" && su::clockText(-3) == "0:00");
    CHECK(hasSound(SourceKind::Media) && hasSound(SourceKind::InputAudio) && !hasSound(SourceKind::Image));
    CHECK(su::gifDelayMs(0) == 100 && su::gifDelayMs(1) == 100 && su::gifDelayMs(4) == 40);
    CHECK(kindFromKey("slideshow") == SourceKind::Slideshow && std::string(kindKey(SourceKind::Slideshow)) == "slideshow");
}

static void testSceneFiles() {
    std::printf("scene folders and files\n");
    SceneCollection c;
    const Id a = c.current(), b = c.addScene("Starting soon"), d = c.addScene("Gameplay"), e = c.addScene("Ending");
    CHECK(c.setSceneFolder(b, " Breaks ") && c.scene(b)->folder == "Breaks");
    CHECK(c.setSceneFolder(e, "Breaks"));
    // Folders keep their scenes together: Ending moved up next to Starting soon.
    std::vector<Id> order;
    for (auto& s : c.scenes()) order.push_back(s.id);
    CHECK(order.size() == 4 && order[2] == b && order[3] == e);
    CHECK(c.setSceneFolder(b, "") && c.scene(b)->folder.empty() && c.scenes().back().id == b);
    SceneCollection r;
    CHECK(r.fromJson(c.toJson()) && r.scene(e)->folder == "Breaks");
    // Export: the scene, a scene inside it, their sources; nothing else.
    const Id cam = c.addSource(SourceKind::Camera, "Face cam", R"({"device":"x"})");
    const Id img = c.addSource(SourceKind::Image, "Logo", R"({"file":"C:\\logo.png"})");
    const Id other = c.addSource(SourceKind::Display, "Display 1");
    Transform t;
    t.x = 960;
    t.y = 540;
    t.width = 480;
    t.height = 270;
    c.addItem(d, cam, t);
    c.addItem(e, img);
    c.addItem(d, c.sceneSource(e));
    c.addItem(a, other);
    const std::string file = c.exportScene(d);
    CHECK(file.find("Face cam") != std::string::npos && file.find("Logo") != std::string::npos && file.find("Display 1") == std::string::npos);
    CHECK(c.exportScene("nope").empty());
    // Import into another collection at 1280x720: new ids, names kept (or made unique), nesting rewired, scaled.
    SceneCollection other2;
    other2.canvasWidth = 1280;
    other2.canvasHeight = 720;
    other2.addScene("Gameplay");
    std::string err;
    bool scaled = false;
    const Id got = other2.importScene(file, err, &scaled);
    CHECK(!got.empty() && err.empty() && scaled);
    const Scene* g = other2.scene(got);
    CHECK(g && g->name == "Gameplay 2" && g->items.size() == 2);
    CHECK(std::fabs(g->items[0].transform.x - 640) < 0.01f && std::fabs(g->items[0].transform.width - 320) < 0.01f);
    const Source* nested = other2.source(g->items[1].source);
    CHECK(nested && nested->kind == SourceKind::Scene && other2.scene(SceneCollection::nestedScene(*nested)) &&
          other2.scene(SceneCollection::nestedScene(*nested))->name == "Ending");
    // Importing twice: everything new again, nothing shared with the first copy. (The first copy's
    // source is noted first: importing adds scenes, which can move the ones already there.)
    const Id firstSource = g->items[0].source;
    const Id got2 = other2.importScene(file, err);
    CHECK(!got2.empty() && got2 != got && other2.scene(got2)->items[0].source != firstSource);
    CHECK(other2.importScene("{\"format\":\"something\"}", err).empty() && !err.empty());
    CHECK(other2.importScene("not json", err).empty());
}

static void testGroups() {
    std::printf("groups\n");
    SceneCollection c;
    const Id sc = c.current();
    const Id a = c.addSource(SourceKind::Image, "Logo"), b = c.addSource(SourceKind::Text, "Title"), d = c.addSource(SourceKind::Camera, "Cam");
    Transform ta, tb;
    ta.x = 100;
    ta.y = 50;
    tb.x = 300;
    tb.y = 60;
    tb.width = 400;
    tb.height = 100;
    const Id ia = c.addItem(sc, a, ta), ib = c.addItem(sc, b, tb), id = c.addItem(sc, d);
    std::string err;
    CHECK(c.groupItems(sc, {ia}, err).empty() && !err.empty());
    const Id g = c.groupItems(sc, {ia, ib}, err);
    CHECK(!g.empty() && c.isGroupItem(sc, g) && !c.isGroupItem(sc, id));
    CHECK(c.scene(sc)->items.size() == 2 && c.scene(sc)->items[0].id == g && c.scene(sc)->items[1].id == id);
    const Id inner = SceneCollection::nestedScene(*c.source(c.item(sc, g)->source));
    CHECK(c.scene(inner)->folder == "Groups" && c.scene(inner)->items.size() == 2 && c.scene(inner)->items[1].transform.x == 300);
    // Move and halve the group, then put the camera in it: it keeps showing where it was.
    Transform gt;
    gt.x = 960;
    gt.y = 540;
    gt.width = 960;
    gt.height = 540;
    CHECK(c.setTransform(sc, g, gt));
    Transform cam;
    cam.x = 1200;
    cam.y = 600;
    cam.width = 200;
    cam.height = 100;
    c.setTransform(sc, id, cam);
    CHECK(c.addToGroup(sc, g, id, err));
    const SceneItem& moved = c.scene(inner)->items.back();
    CHECK(std::fabs(moved.transform.x - 480) < 0.01f && std::fabs(moved.transform.width - 400) < 0.01f);
    // Ungroup: everything back where it showed.
    CHECK(c.ungroup(sc, g, err));
    CHECK(c.scene(sc)->items.size() == 3 && !c.scene(inner));
    const SceneItem* back = c.item(sc, ib);
    CHECK(back && std::fabs(back->transform.x - (960 + 150)) < 0.01f && std::fabs(back->transform.width - 200) < 0.01f);
    const SceneItem* camBack = c.item(sc, id);
    CHECK(camBack && std::fabs(camBack->transform.x - 1200) < 0.01f && std::fabs(camBack->transform.width - 200) < 0.01f);
    // A rotated group can't be undone exactly: refused.
    const Id g2 = c.groupItems(sc, {ia, ib}, err);
    Transform rot;
    rot.rotation = 15;
    c.setTransform(sc, g2, rot);
    CHECK(!c.ungroup(sc, g2, err) && !err.empty() && c.isGroupItem(sc, g2));
}

namespace dsptest {
std::vector<float> sine(float hz, float amp, std::size_t frames, float phase = 0) {
    std::vector<float> v(frames * 2);
    for (std::size_t i = 0; i < frames; ++i) v[i * 2] = v[i * 2 + 1] = amp * std::sin(phase + 2 * 3.14159265f * hz * (float)i / 48000.0f);
    return v;
}
std::vector<float> noise(float amp, std::size_t frames, unsigned seed = 1) {
    std::vector<float> v(frames * 2);
    for (auto& x : v) {
        seed = seed * 1664525u + 1013904223u;
        x = amp * ((float)(seed >> 8) / 8388608.0f - 1.0f);
    }
    return v;
}
float rms(const std::vector<float>& v, std::size_t from, std::size_t to, int ch = 0) {
    double s = 0;
    for (std::size_t i = from; i < to; ++i) s += (double)v[i * 2 + (std::size_t)ch] * v[i * 2 + (std::size_t)ch];
    return (float)std::sqrt(s / (double)std::max<std::size_t>(1, to - from));
}
void run(ncap::dsp::Processor& p, std::vector<float>& v) {
    for (std::size_t at = 0; at < v.size() / 2; at += 480) p.process(v.data() + at * 2, std::min<std::size_t>(480, v.size() / 2 - at));
}
}  // namespace dsptest


static void testBorder() {
    std::printf("border\n");
    using namespace ncap::border;
    // Distances: the centre is inside, an edge is 0, a rounded corner's outside is outside.
    CHECK(near(distance(50, 25, 100, 50, 0), -25, 1e-4));
    CHECK(near(distance(100, 25, 100, 50, 0), 0, 1e-4));
    CHECK(near(distance(0, 0, 100, 50, 0), 0, 1e-4));
    CHECK(distance(0.5f, 0.5f, 100, 50, 20) > 5);           // cut off by a 20 px corner
    CHECK(near(distance(20, 0, 100, 50, 20), 0, 1e-4));     // where the corner's curve meets the top edge
    CHECK(near(distance(25, 25, 50, 50, 25), -25, 1e-4));   // a circle: its centre is a radius in
    CHECK(near(distance(50, 25, 50, 50, 25), 0, 1e-4));
    CHECK(near(distance(25 + 25 / std::sqrt(2.0f), 25 + 25 / std::sqrt(2.0f), 50, 50, 25), 0, 1e-3));
    CHECK(coverage(-3) == 1 && coverage(3) == 0 && near(coverage(0), 0.5f, 1e-6));
    Style s;
    s.roundness = 100;
    CHECK(near(radius(s, 200, 100), 50, 1e-4));
    s.roundness = 50;
    CHECK(near(radius(s, 200, 100), 25, 1e-4));

    // JSON: kept, and clamped when it's out of range or nonsense.
    Style a = presets()[1].style;
    CHECK(fromJson(toJson(a)) == a);
    Style b = fromJson(R"({"on":true,"width":500,"roundness":-4,"colour":"#ff000080","glow":"x","offsetX":-999,"colour2":"nope"})");
    CHECK(b.on && b.width == 60 && b.roundness == 0 && b.colour.r == 255 && b.colour.a == 128 && b.glow == 0 && b.offsetX == -60 && b.colour2.b == 255);
    CHECK(!fromJson("[]").on);

    // Gradient: one colour at one end, the other at the other.
    Style g;
    g.gradient = true;
    g.colour = {255, 0, 0, 255};
    g.colour2 = {0, 0, 255, 255};
    g.angle = 0;
    CHECK(ringColour(g, 0, 50, 200, 100).r == 255 && ringColour(g, 200, 50, 200, 100).b == 255);
    CHECK(ringColour(g, 100, 50, 200, 100).r == 128 || ringColour(g, 100, 50, 200, 100).r == 127);
    g.angle = 90;   // top to bottom
    CHECK(ringColour(g, 100, 0, 200, 100).r == 255 && ringColour(g, 100, 100, 200, 100).b == 255);

    // The picture: the ring round the outside, the item's own area clear; a glow round that.
    Style r;
    r.on = true;
    r.width = 4;
    r.roundness = 0;
    int w = 0, h = 0;
    auto px = render(r, 40, 20, w, h);
    CHECK(margin(r) == 4 && w == 48 && h == 28 && px.size() == 48u * 28 * 4);
    auto at = [&](int x, int y) { return &px[((size_t)y * (size_t)w + (size_t)x) * 4]; };
    CHECK(at(1, 14)[3] == 255 && at(1, 14)[0] == 255);   // outside the item's left edge: the ring
    CHECK(at(4, 14)[3] == 0 && at(24, 14)[3] == 0);      // the item's own area (its first column, its middle): clear
    CHECK(at(47, 0)[3] == 255);                          // square corner of the ring
    r.roundness = 100;                                   // a pill: the ring's corners are rounded too
    px = render(r, 40, 20, w, h);
    CHECK(at(0, 0)[3] == 0 && at(24, 1)[3] == 255);
    r.glow = 10;
    r.glowOpacity = 100;
    r.offsetY = 5;
    px = render(r, 40, 20, w, h);
    CHECK(margin(r) == 19 && w == 78 && h == 58);
    CHECK(at(39, 29)[3] == 0);                           // the item's area stays clear even with a glow under it
    CHECK(at(39, 19 + 20 + 4 + 3)[3] > at(39, 19 - 4 - 3)[3]);   // the shadow falls more below (offset down) than above
    CHECK(at(0, 0)[3] == 0);                             // far corner: nothing
    r.on = false;
    CHECK(margin(r) == 0);
    px = render(r, 40, 20, w, h);                        // a PNG of a border that's off is drawn as if on
    CHECK(w == 78);

    // Presets: the circle wants a square item; all are on.
    bool circle = false;
    for (auto& p : presets()) {
        CHECK(p.style.on);
        if (p.square) circle = p.style.roundness == 100;
    }
    CHECK(circle);
}

// A stand-in plugin for the chain: halves the sound; only "half.dll" is "switched on".
struct HalfExternal : ncap::dsp::External {
    float g;
    explicit HalfExternal(float g) : g(g) {}
    void process(float* x, std::size_t n) override {
        for (std::size_t i = 0; i < n * 2; ++i) x[i] *= g;
    }
    int latency() const override { return 7; }
};
static std::unique_ptr<ncap::dsp::External> halfFactory(const std::string& plugin, const std::map<std::string, float>& v) {
    if (plugin != "half.dll") return nullptr;
    auto it = v.find("gain");
    return std::make_unique<HalfExternal>(it == v.end() ? 0.5f : it->second);
}

static void testPlugins() {
    std::printf("plugins\n");
    using namespace ncap::plugins;
    // File names.
    CHECK(validFileName("tremolo.dll") && validFileName("My Effect 2.DLL"));
    CHECK(!validFileName("..\\evil.dll") && !validFileName("a/b.dll") && !validFileName("c:x.dll") && !validFileName("x.exe"));
    CHECK(!validFileName("con.dll") && !validFileName("NUL.dll") && !validFileName(".dll") && !validFileName(""));
    CHECK(!validFileName(std::string(130, 'a') + ".dll"));
    const std::uint8_t b[] = {0x00, 0x7f, 0xab, 0xff};
    CHECK(hex(b, 4) == "007fabff");

    // Nothing is on until enabled; enabling ties it to the hash; a changed file turns it off.
    const std::string h1(64, 'a'), h2(64, 'b');
    Registry r;
    CHECK(reconcile(r, {{"tremolo.dll", h1, 100}}).empty());
    CHECK(!r.find("tremolo.dll"));
    enable(r, {"tremolo.dll", h1, 100});
    CHECK(r.find("tremolo.dll")->enabled);
    CHECK(reconcile(r, {{"tremolo.dll", h1, 100}}).empty() && r.find("tremolo.dll")->enabled);
    auto msgs = reconcile(r, {{"TREMOLO.dll", h2, 100}});
    CHECK(msgs.size() == 1 && !r.find("tremolo.dll")->enabled && r.find("tremolo.dll")->offReason.find("changed") != std::string::npos);
    enable(r, {"tremolo.dll", h2, 100});
    CHECK(r.find("tremolo.dll")->enabled && r.find("tremolo.dll")->offReason.empty());
    msgs = reconcile(r, {});
    CHECK(msgs.size() == 1 && !r.find("tremolo.dll")->enabled && r.find("tremolo.dll")->offReason.find("no longer") != std::string::npos);

    // Saved and read back; a bad hash can't stay enabled; bad names dropped.
    enable(r, {"tremolo.dll", h2, 100});
    enable(r, {"other.dll", h1, 5});
    disable(r, "other.dll");
    Registry r2 = fromJson(toJson(r));
    CHECK(r2.records.size() == 2 && r2.find("tremolo.dll")->enabled && r2.find("tremolo.dll")->sha256 == h2 && !r2.find("other.dll")->enabled);
    r2 = fromJson(R"({"plugins":[{"file":"x.dll","sha256":"zz","enabled":true},{"file":"..\\y.dll","sha256":")" + h1 +
                  R"(","enabled":true},{"file":"x.dll","sha256":")" + h1 + R"(","enabled":true}]})");
    CHECK(r2.records.size() == 1 && !r2.records[0].enabled);
    CHECK(fromJson("not json").records.empty());

    // After a crash, the plugins that were running are switched off.
    enable(r, {"other.dll", h1, 5});
    const auto running = runningFromText(runningToText({"tremolo.dll", "..\\bad.dll"}) + "other.dll\r\n");
    CHECK(running.size() == 2);
    msgs = disableAfterCrash(r, running);
    CHECK(msgs.size() == 2 && !r.find("tremolo.dll")->enabled && !r.find("other.dll")->enabled);

    // A plugin's own description.
    Info i;
    i.api = 1;
    i.name = "Tremolo";
    i.hasCreate = i.hasDestroy = i.hasSetParam = i.hasProcess = true;
    i.params = {{"rate", "Rate", "Hz", 0.5f, 20, 5}, {"depth", "Depth", "%", 0, 100, 50}};
    std::string why;
    CHECK(validate(i, why));
    Info bad = i;
    bad.api = 2;
    CHECK(!validate(bad, why) && why.find("version 2") != std::string::npos);
    bad = i;
    bad.hasProcess = false;
    CHECK(!validate(bad, why));
    bad = i;
    bad.params[1].key = "rate";
    CHECK(!validate(bad, why));
    bad = i;
    bad.params[0].def = 30;
    CHECK(!validate(bad, why));
    bad = i;
    bad.params[0].max = NAN;
    CHECK(!validate(bad, why));
    bad = i;
    bad.params.resize(17, {"k", "K", "", 0, 1, 0});
    CHECK(!validate(bad, why));

    // In a chain: a plugin filter runs through the host's factory; one that isn't on passes sound through.
    using namespace ncap::dsp;
    Filter pf = Filter::make(Type::Plugin);
    pf.plugin = "half.dll";
    pf.values["gain"] = 0.25f;
    Filter off = pf;
    off.plugin = "missing.dll";
    const Chain chain = ncap::dsp::fromJson(ncap::dsp::toJson(Chain{pf, off}));
    CHECK(chain.size() == 2 && chain[0].type == Type::Plugin && chain[0].plugin == "half.dll" && chain[0].get("gain") == 0.25f);
    CHECK(ncap::dsp::fromJson(R"([{"type":"plugin","plugin":"..\\x.dll"},{"type":"plugin"}])").empty());
    {
        Processor none(chain);   // no host yet: neither runs
        CHECK(none.notRunning().size() == 2);
    }
    setExternalFactory(halfFactory);
    Processor p(chain);
    CHECK(p.notRunning().size() == 1 && p.notRunning()[0] == "missing.dll" && p.latencyFrames() == 7);
    float x[4] = {1, -1, 0.5f, 0.5f};
    p.process(x, 2);
    CHECK(x[0] == 0.25f && x[1] == -0.25f && x[2] == 0.125f);
    setExternalFactory(nullptr);
}


static void testDesign() {
    std::printf("designs\n");
    using namespace ncap::design;
    // Gradients: the ends, a middle stop, between stops.
    Fill g = Fill::linear({{0, {0, 0, 0, 255}}, {0.5f, {255, 0, 0, 255}}, {1, {255, 255, 255, 255}}}, 0);
    CHECK(colourAt(g, 0).r == 0 && colourAt(g, 0.5f).r == 255 && colourAt(g, 0.5f).g == 0 && colourAt(g, 1).g == 255);
    CHECK(colourAt(g, 0.25f).r == 128 || colourAt(g, 0.25f).r == 127);
    CHECK(colourAt(Fill::none(), 0.5f).a == 0 && colourAt(Fill::solid({1, 2, 3, 4}), 0.9f).b == 3);

    // Editing: ids, order, duplicate just above, layers.
    Document d;
    Element r;
    r.kind = Kind::Rect;
    r.x = 100, r.y = 100, r.w = 200, r.h = 100;
    const std::string a = add(d, r);
    Element t;
    t.kind = Kind::Text;
    t.text = "Hello";
    t.x = 500, t.y = 500, t.w = 300, t.h = 80;
    const std::string b = add(d, t);
    CHECK(a == "e1" && b == "e2" && d.elements.size() == 2 && d.find(a)->name == "Rectangle");
    const std::string c = duplicate(d, a);
    CHECK(c == "e3" && d.indexOf(c) == 1 && d.find(c)->x == 120 && d.find(c)->name == "Rectangle copy");
    CHECK(toTop(d, a) && d.indexOf(a) == 2 && toBottom(d, a) && d.indexOf(a) == 0 && !toBottom(d, a));
    CHECK(move(d, a, 1) && d.indexOf(a) == 1 && !move(d, b, 5));
    CHECK(remove(d, c) && !d.find(c) && !remove(d, "nope"));

    // Hit-testing: topmost first, rotation honoured, hidden and locked skipped, ellipses round.
    CHECK(hitTest(d, 150, 150) == a && hitTest(d, 600, 520) == b && hitTest(d, 50, 50).empty());
    d.find(b)->locked = true;
    CHECK(hitTest(d, 600, 520).empty());
    d.find(b)->locked = false;
    Element rot = r;
    rot.x = 1000, rot.y = 100, rot.w = 400, rot.h = 40, rot.rotation = 90;   // standing up: 40 wide, 400 tall about (1200, 120)
    const std::string rid = add(d, rot);
    CHECK(hitTest(d, 1200, 290) == rid && hitTest(d, 1390, 120).empty());
    Element el = r;
    el.kind = Kind::Ellipse;
    el.x = 0, el.y = 600, el.w = 200, el.h = 200;
    const std::string eid = add(d, el);
    CHECK(hitTest(d, 100, 700) == eid && hitTest(d, 5, 605).empty());   // the corner of its box is outside the circle

    // JSON: the same document back; nonsense dropped and clamped.
    d.background = g;
    d.find(b)->shadow = {true, 3, 4, 10, {0, 0, 0, 100}};
    d.find(b)->anim = Anim::SlideUp;
    d.find(b)->bold = true;
    const Document back = fromJson(toJson(d));
    CHECK(back.elements.size() == d.elements.size() && back.background == d.background && back.find(b)->shadow == d.find(b)->shadow);
    CHECK(back.find(b)->anim == Anim::SlideUp && back.find(b)->bold && back.find(b)->text == "Hello" && back.find(rid)->rotation == 90);
    CHECK(back.elements == d.elements);
    Element e4;
    e4.kind = Kind::Rect;
    CHECK(add(*const_cast<Document*>(&back), e4) == "e" + std::to_string(d.nextId));   // ids keep counting
    const Document bad = fromJson(R"({"width":99999,"height":-5,"elements":[{"kind":"blob","id":"x"},{"kind":"rect","id":"e9","opacity":7,"w":-3},{"kind":"rect","id":"e9"},{"kind":"text"}],"background":{"type":"linear","stops":[{"at":0,"colour":"#000"}]}})");
    CHECK(bad.width == 8192 && bad.height == 16 && bad.elements.size() == 1 && bad.elements[0].opacity == 1 && bad.elements[0].w == 1);
    CHECK(bad.background.type == Fill::None && bad.nextId == 10);

    // Snapping: to the page centre, to another element's edge, only within the threshold.
    Document s;
    s.width = 1000, s.height = 500;
    Element o = r;
    o.x = 600, o.y = 50, o.w = 100, o.h = 100;
    add(s, o);
    Snapped sn = snap(s, "", {447, 300, 100, 50}, 6);
    CHECK(near(sn.box.x, 450) && sn.guidesX.size() == 1 && near(sn.guidesX[0], 500));   // centre at 497 → 500
    sn = snap(s, "", {200, 147, 100, 50}, 6);
    CHECK(near(sn.box.y, 150) && near(sn.guidesY[0], 150));   // top meets the other's bottom (150)
    sn = snap(s, "", {200, 300, 100, 50}, 6);
    CHECK(sn.guidesX.empty() && sn.guidesY.empty() && near(sn.box.x, 200));
    sn = snap(s, "", {400, 300, 197, 50}, 6, true);   // resizing: the right edge to 600
    CHECK(near(sn.box.w, 200) && near(sn.box.x, 400));

    // Entrances.
    Element an = r;
    an.anim = Anim::Fade;
    an.animMs = 400;
    CHECK(near(pose(an, 0, 1920, 1080).opacity, 0) && near(pose(an, 400, 1920, 1080).opacity, 1) && pose(an, 200, 1920, 1080).opacity > 0.5f);
    an.anim = Anim::SlideLeft;
    CHECK(pose(an, 0, 1920, 1080).dx <= -(an.x + an.w) + 0.01f && near(pose(an, 400, 1920, 1080).dx, 0));
    an.anim = Anim::Pop;
    float peak = 0;
    for (int ms = 0; ms <= 400; ms += 10) peak = std::max(peak, pose(an, (float)ms, 1920, 1080).scale);
    CHECK(peak > 1.05f && near(pose(an, 400, 1920, 1080).scale, 1));
    an.anim = Anim::Wipe;
    CHECK(near(pose(an, 0, 1920, 1080).reveal, 0) && near(pose(an, 1000, 1920, 1080).reveal, 1));
    Document ad;
    add(ad, an);
    CHECK(animating(ad, 100) && !animating(ad, 450));

    // Templates: all valid and round-trip; placeholders.
    CHECK(templates().size() >= 8);
    for (auto& tp : templates()) {
        CHECK(!tp.doc.elements.empty());
        CHECK(fromJson(toJson(tp.doc)).elements == tp.doc.elements);
    }
    CHECK(fillPlaceholders("{twitch} and {twitch} / {x}", {{"twitch", "twitch.tv/cj"}}) == "twitch.tv/cj and twitch.tv/cj / {x}");
    // Parts between dots whose details aren't set are dropped with their dots; the designer shows the tags.
    const std::vector<std::pair<std::string, std::string>> vals = {{"twitch", "cjphilip"}, {"youtube", ""}, {"instagram", "@cj"}};
    CHECK(fillPlaceholders("{twitch}   \u00b7   {youtube}   \u00b7   {instagram}", vals) == "cjphilip   \u00b7   @cj");
    CHECK(fillPlaceholders("{youtube} \u00b7 {youtube}", vals).empty());
    CHECK(fillPlaceholders("{twitch} \u00b7 {youtube}", vals, true) == "cjphilip   \u00b7   {youtube}");
    CHECK(fillPlaceholders("Line one\nTwitch: {twitch}", vals) == "Line one\nTwitch: cjphilip");
    // Radial fills.
    const Fill rad = Fill::radial({{0, {255, 255, 255, 200}}, {1, {255, 255, 255, 0}}});
    CHECK(colourAt(rad, 0).a == 200 && colourAt(rad, 1).a == 0 && fromJson(toJson([&] { Document x; x.background = rad; return x; }())).background == rad);
}


static void testTrackNames() {
    std::printf("track names\n");
    using ncap::sourceutil::trackFromFileName;
    using T = ncap::sourceutil::TrackInfo;
    CHECK(trackFromFileName("C:\\Music\\Aurora Band - Night Sky.mp3") == (T{"Night Sky", "Aurora Band"}));
    CHECK(trackFromFileName("03 - Calm Waters.flac") == (T{"Calm Waters", ""}));
    CHECK(trackFromFileName("03. Lofi Girl - Snowfall.m4a") == (T{"Snowfall", "Lofi Girl"}));
    CHECK(trackFromFileName("/home/x/Quiet_Reading_Loop.wav") == (T{"Quiet Reading Loop", ""}));
    CHECK(trackFromFileName("1999 Song.mp3") == (T{"1999 Song", ""}));
    CHECK(trackFromFileName("Plain") == (T{"Plain", ""}));
    CHECK(trackFromFileName("Artist - .mp3") == (T{"Artist", ""}));
    // Designs that show only while music plays.
    namespace dz = ncap::design;
    const dz::Template* np = nullptr;
    for (auto& tp : dz::templates())
        if (std::string(tp.id) == "now-playing") np = &tp;
    CHECK(np && np->doc.onlyWhilePlaying && dz::usesBrand(np->doc));
    CHECK(np && dz::fromJson(dz::toJson(np->doc)).onlyWhilePlaying && !dz::fromJson(dz::toJson(dz::Document{})).onlyWhilePlaying);
    CHECK(dz::fillPlaceholders("{song} \u00B7 {artist}", {{"song", "Snowfall"}, {"artist", ""}}) == "Snowfall");
}


// A minimal GIF decoder for the test: the global palette, each frame's delay, transparency and pixels.
namespace {
struct GifOut {
    int w = 0, h = 0, frames = 0, loop = -1;
    std::vector<int> delays;
    std::vector<std::vector<int>> idx;
    std::vector<std::array<int, 3>> pal;
    int clear = -1;
};
bool gifDecode(const std::vector<std::uint8_t>& g, GifOut& o) {
    if (g.size() < 13 || std::memcmp(g.data(), "GIF89a", 6) != 0) return false;
    std::size_t p = 6;
    auto u16 = [&](std::size_t at) { return g[at] | (g[at + 1] << 8); };
    o.w = u16(p), o.h = u16(p + 2);
    const int flags = g[p + 4];
    p += 7;
    const int n = 1 << ((flags & 7) + 1);
    for (int i = 0; i < n; ++i, p += 3) o.pal.push_back({g[p], g[p + 1], g[p + 2]});
    int delay = 0;
    while (p < g.size()) {
        const int b = g[p++];
        if (b == 0x3B) return true;
        if (b == 0x21) {
            const int label = g[p++];
            if (label == 0xF9) {
                delay = u16(p + 2);
                if (g[p + 1] & 1) o.clear = g[p + 4];
            } else if (label == 0xFF && std::memcmp(&g[p + 1], "NETSCAPE2.0", 11) == 0) o.loop = u16(p + 14);
            while (g[p]) p += g[p] + 1;
            ++p;
            continue;
        }
        if (b != 0x2C) return false;
        const int fw = u16(p + 4), fh = u16(p + 6);
        p += 9;
        const int minCode = g[p++];
        std::vector<std::uint8_t> data;
        while (g[p]) {
            data.insert(data.end(), g.begin() + (long)p + 1, g.begin() + (long)p + 1 + g[p]);
            p += g[p] + 1;
        }
        ++p;
        // LZW
        std::vector<std::vector<int>> dict;
        const int clearCode = 1 << minCode, end = clearCode + 1;
        int size = minCode + 1;
        auto resetDict = [&] {
            dict.clear();
            for (int i = 0; i < clearCode + 2; ++i) dict.push_back({i});
            size = minCode + 1;
        };
        resetDict();
        std::vector<int> out, prev;
        std::size_t bit = 0;
        for (;;) {
            if (bit + size > data.size() * 8) break;
            int code = 0;
            for (int i = 0; i < size; ++i, ++bit) code |= ((data[bit / 8] >> (bit % 8)) & 1) << i;
            if (code == clearCode) {
                resetDict();
                prev.clear();
                continue;
            }
            if (code == end) break;
            std::vector<int> entry;
            if (code < (int)dict.size()) entry = dict[(size_t)code];
            else if (code == (int)dict.size() && !prev.empty()) entry = prev, entry.push_back(prev[0]);
            else return false;
            out.insert(out.end(), entry.begin(), entry.end());
            if (!prev.empty() && dict.size() < 4096) {
                auto ne = prev;
                ne.push_back(entry[0]);
                dict.push_back(ne);
                if ((int)dict.size() == (1 << size) && size < 12) ++size;
            }
            prev = entry;
        }
        if ((int)out.size() != fw * fh) return false;
        o.idx.push_back(out);
        o.delays.push_back(delay);
        ++o.frames;
    }
    return false;
}
}  // namespace

static void testGif() {
    std::printf("animated GIFs\n");
    const int w = 37, h = 23;   // odd sizes
    std::vector<gif::Frame> frames;
    for (int f = 0; f < 5; ++f) {
        gif::Frame fr;
        fr.bgra.resize((size_t)w * h * 4);
        fr.delayMs = 40;
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                std::uint8_t* px = &fr.bgra[((size_t)y * w + x) * 4];
                // Four flat colours (exactly representable) and a see-through stripe that moves.
                const int q = (x < w / 2 ? 0 : 1) + (y < h / 2 ? 0 : 2);
                const std::uint8_t cols[4][3] = {{255, 0, 0}, {0, 255, 0}, {0, 0, 255}, {240, 240, 240}};
                px[2] = cols[q][0], px[1] = cols[q][1], px[0] = cols[q][2];
                px[3] = (x == f * 3) ? 0 : 255;
            }
        frames.push_back(fr);
    }
    frames.push_back(frames.back());   // repeats merge into one longer frame
    frames.back().delayMs = 1000;
    const auto g = gif::encode(w, h, frames, false);
    GifOut o;
    CHECK(gifDecode(g, o));
    CHECK(o.w == w && o.h == h);
    CHECK(o.frames == 5);
    CHECK(o.loop == 0);
    CHECK(o.delays.size() == 5 && o.delays[0] == 4 && o.delays[4] == 104);
    CHECK(o.clear >= 0);
    bool exact = true, seeThrough = true;
    for (int f = 0; f < o.frames && f < 5; ++f)
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const int i = o.idx[(size_t)f][(size_t)(y * w + x)];
                const std::uint8_t* px = &frames[(size_t)f].bgra[((size_t)y * w + x) * 4];
                if (px[3] == 0) {
                    seeThrough &= i == o.clear;
                    continue;
                }
                auto c = o.pal[(size_t)i];
                exact &= c[0] == px[2] && c[1] == px[1] && c[2] == px[0];
            }
    CHECK(exact);
    CHECK(seeThrough);
    // Many colours (more than 256, and long enough for the LZW table to fill and restart).
    gif::Frame big;
    const int bw = 300, bh = 200;
    big.bgra.resize((size_t)bw * bh * 4);
    std::uint32_t seed = 7;
    for (std::size_t i = 0; i < (size_t)bw * bh; ++i) {
        seed = seed * 1664525u + 1013904223u;
        big.bgra[i * 4] = (std::uint8_t)(seed >> 24), big.bgra[i * 4 + 1] = (std::uint8_t)(seed >> 16), big.bgra[i * 4 + 2] = (std::uint8_t)(seed >> 8);
        big.bgra[i * 4 + 3] = 255;
    }
    GifOut ob;
    CHECK(gifDecode(gif::encode(bw, bh, {big}), ob) && ob.frames == 1 && ob.loop == -1 && ob.clear == -1);
    CHECK(ob.pal.size() == 256);
    CHECK(gif::encode(0, 5, {big}).empty() && gif::encode(bw, bh, {}).empty());
    gif::Frame wrong;
    wrong.bgra.resize(10);
    CHECK(gif::encode(bw, bh, {wrong}).empty());
    // How long a design's entrance takes.
    design::Document d;
    CHECK(design::entranceMs(d) == 0);
    design::Element e;
    e.anim = design::Anim::Fade;
    e.animMs = 700;
    d.elements.push_back(e);
    e.animMs = 1200;
    e.visible = false;
    d.elements.push_back(e);
    CHECK(design::entranceMs(d) == 700);
}


static void testChecklist() {
    std::printf("pre-live checklist\n");
    namespace cl = ncap::checklist;
    CHECK(cl::defaultItems().size() == 4);
    const auto t = cl::tidy({"  Phone on silent ", "", "   ", "Phone on silent", "Water", std::string(100, 'x'), std::string(79, 'y') + "\xC3\xA9z"});
    CHECK(t.size() == 4 && t[0] == "Phone on silent" && t[1] == "Water");
    CHECK(t[2].size() == cl::kMaxChars);
    CHECK(t[3] == std::string(79, 'y'));   // not cut inside the é
    std::vector<std::string> many;
    for (int i = 0; i < 20; ++i) many.push_back("item " + std::to_string(i));
    CHECK(cl::tidy(many).size() == cl::kMaxItems);
    auto has = [](const std::vector<cl::Check>& c, cl::State st, const std::string& part) {
        for (auto& x : c)
            if (x.state == st && x.text.find(part) != std::string::npos) return true;
        return false;
    };
    // All good.
    cl::Snapshot s;
    s.destinations = {{"Twitch", true, 0}, {"YouTube", true, 0}};
    s.scene = "Starting Soon";
    s.mics = {{"Shure MV7", false, 1, 2.0, true}};
    auto c = cl::run(s);
    CHECK(has(c, cl::State::Ok, "Going live on Twitch and YouTube."));
    CHECK(has(c, cl::State::Ok, "Viewers will see Starting Soon first"));
    CHECK(has(c, cl::State::Ok, "Shure MV7 is picking you up"));
    CHECK(!cl::anyProblem(c));
    // The mic: quiet, never heard, muted, not routed, not working, none.
    s.mics[0].silentFor = 30;
    CHECK(has(cl::run(s), cl::State::Waiting, "Say something to check Shure MV7"));
    CHECK(!cl::anyProblem(cl::run(s)));   // waiting isn't a problem
    s.mics[0].silentFor = -1;
    CHECK(has(cl::run(s), cl::State::Waiting, "Say something"));
    s.mics[0].muted = true;
    CHECK(has(cl::run(s), cl::State::Warn, "is muted") && cl::anyProblem(cl::run(s)));
    s.mics[0].muted = false;
    s.mics[0].buses = 1u << 3;   // only Track 3
    CHECK(has(cl::run(s), cl::State::Warn, "isn't in the sound your stream carries"));
    s.destinations[1].bus = 3;   // YouTube carries Track 3
    CHECK(!has(cl::run(s), cl::State::Warn, "isn't in the sound"));
    s.mics[0].working = false;
    CHECK(has(cl::run(s), cl::State::Problem, "isn't working"));
    s.mics.clear();
    CHECK(has(cl::run(s), cl::State::Warn, "no microphone"));
    // Destinations: a missing key, none at all.
    s.destinations = {{"Twitch", true, 0}, {"Kick", false, 0}};
    c = cl::run(s);
    CHECK(has(c, cl::State::Ok, "Going live on Twitch.") && has(c, cl::State::Problem, "Kick has no stream key, so it won't start"));
    s.destinations = {{"Kick", false, 0}, {"Custom", false, 0}};
    CHECK(has(cl::run(s), cl::State::Problem, "Kick and Custom have no stream key, so they won't start"));
    cl::Destination kick{"Kick", false, 0};
    kick.hasServer = false;
    s.destinations = {{"Twitch", true, 0}, kick};
    c = cl::run(s);
    CHECK(has(c, cl::State::Problem, "Kick has no server address yet") && !has(c, cl::State::Problem, "stream key"));
    s.destinations.clear();
    CHECK(has(cl::run(s), cl::State::Problem, "No destination is switched on"));
    // The scene and the encoder.
    s.sceneEmpty = true;
    CHECK(has(cl::run(s), cl::State::Warn, "Starting Soon, is empty"));
    s.encodersKnown = false;
    CHECK(has(cl::run(s), cl::State::Waiting, "Checking this PC's video encoders"));
    s.encodersKnown = true;
    s.hardwareEncoder = false;
    CHECK(has(cl::run(s), cl::State::Warn, "No hardware video encoder"));
    s.recording = true;
    CHECK(has(cl::run(s), cl::State::Ok, "Recording is already on"));
}


static void testAutomation() {
    std::printf("automation\n");
    namespace au = ncap::automation;
    CHECK(au::normaliseExe("C:\\Program Files\\Krita\\bin\\Krita.EXE") == "krita.exe");
    CHECK(au::normaliseExe(" krita ") == "krita.exe");
    CHECK(au::normaliseExe("\"WritingStudio.exe\"") == "writingstudio.exe");
    CHECK(au::normaliseExe("/usr/bin/") == "" && au::normaliseExe("   ") == "" && au::normaliseExe(".exe") == "");
    au::Rule krita{"krita.exe", "Krita", au::When::Opens, "Art"};
    au::Rule game{"eldenring.exe", "", au::When::Closes, "Just chatting"};
    CHECK(au::describe(krita) == "When Krita opens, switch to Art");
    game.onlyLive = true;
    CHECK(au::describe(game) == "When eldenring.exe closes, switch to Just chatting, while live or recording");
    // JSON round trip, and bad entries skipped.
    const std::vector<au::Rule> rules = {krita, game};
    CHECK(au::fromJson(au::toJson(rules)) == rules);
    const auto bad = au::fromJson(R"([{"exe":"","when":"opens","scene":"A"},{"exe":"a","when":"later","scene":"A"},{"exe":"a","when":"opens"},5,
                                       {"exe":"Paint.exe","when":"closes","scene":"B","on":false}])");
    CHECK(bad.size() == 1 && bad[0].exe == "paint.exe" && bad[0].when == au::When::Closes && !bad[0].on);
    CHECK(au::fromJson("not json").empty() && au::fromJson("{}").empty());
    std::string many = "[";
    for (int i = 0; i < 30; ++i) many += std::string(i ? "," : "") + R"({"exe":"a.exe","when":"opens","scene":"S"})";
    CHECK(au::fromJson(many + "]").size() == au::kMaxRules);
    // The watcher: nothing on the first look, then changes only.
    au::Watcher w;
    std::vector<au::Rule> rs = {krita, game};
    rs[1].onlyLive = false;
    CHECK(w.update({"explorer.exe", "krita.exe", "eldenring.exe"}, rs, false) == -1);   // already open at start
    CHECK(w.update({"explorer.exe", "krita.exe", "eldenring.exe"}, rs, false) == -1);   // nothing changed
    CHECK(w.update({"explorer.exe", "krita.exe"}, rs, false) == 1);                     // the game closed
    CHECK(w.update({"explorer.exe"}, rs, false) == -1);                                 // Krita closing has no rule
    CHECK(w.update({"explorer.exe", "krita.exe"}, rs, false) == 0);                     // Krita opened
    CHECK(w.update({"explorer.exe", "krita.exe"}, rs, false) == -1);                    // still open: once only
    // Both at once: the last in the list wins.
    CHECK(w.update({"eldenring.exe"}, rs, false) == -1);   // Krita closed, the game opened: no rule for either
    CHECK(w.update({"krita.exe"}, rs, false) == 1);        // the game closed and Krita opened together
    // Off, and only while live.
    rs[0].on = false;
    w.update({}, rs, false);
    CHECK(w.update({"krita.exe"}, rs, false) == -1);
    rs[0].on = true;
    rs[0].onlyLive = true;
    w.update({}, rs, false);
    CHECK(w.update({"krita.exe"}, rs, false) == -1);
    w.update({}, rs, true);
    CHECK(w.update({"krita.exe"}, rs, true) == 0);
    w.reset();
    CHECK(w.update({}, rs, true) == -1);   // after a reset, the first look only notes what's running
}

static void testAuroraSweep() {
    std::printf("aurora sweep\n");
    namespace tr = ncap::transition;
    tr::Kind k;
    CHECK(tr::parse("aurora", k) && k == tr::Kind::Aurora && std::string(tr::key(tr::Kind::Aurora)) == "aurora");
    tr::Settings st;
    st.kind = tr::Kind::Aurora;
    st.direction = tr::Direction::Right;
    CHECK(tr::fromJson(tr::toJson(st)).kind == tr::Kind::Aurora);
    for (auto d : {tr::Direction::Left, tr::Direction::Right, tr::Direction::Up, tr::Direction::Down}) {
        // At the start the old scene everywhere and no light; at the end the new scene and no light.
        float maxStartTo = 0, maxStartLight = 0, minEndTo = 1, maxEndLight = 0;
        for (float u = 0; u <= 1.0001f; u += 0.05f)
            for (float v = 0; v <= 1.0001f; v += 0.05f) {
                const auto a = tr::aurora(u, v, 0, d), b = tr::aurora(u, v, 1, d);
                maxStartTo = std::max(maxStartTo, a.to), maxStartLight = std::max(maxStartLight, a.light);
                minEndTo = std::min(minEndTo, b.to), maxEndLight = std::max(maxEndLight, b.light);
            }
        CHECK(maxStartTo < 0.001f && maxStartLight < 0.02f && minEndTo > 0.999f && maxEndLight < 0.02f);
        // Halfway: a band of light crossing the middle, the new scene on the side it came from.
        float mid = 0;
        for (float t = 0; t <= 1.0001f; t += 0.01f) mid = std::max(mid, tr::aurora(t, t, 0.5f, d).light);
        CHECK(mid > 0.5f);
    }
    // Moving right: the left side turns over first.
    CHECK(tr::aurora(0.1f, 0.5f, 0.45f, tr::Direction::Right).to > 0.9f && tr::aurora(0.9f, 0.5f, 0.45f, tr::Direction::Right).to < 0.1f);
    CHECK(tr::aurora(0.9f, 0.5f, 0.45f, tr::Direction::Left).to > 0.9f && tr::aurora(0.1f, 0.5f, 0.45f, tr::Direction::Left).to < 0.1f);
    // Over time each point only goes from old to new.
    bool monotonic = true;
    float prev = 0;
    for (float p = 0; p <= 1.0001f; p += 0.01f) {
        const float to = tr::aurora(0.3f, 0.7f, p, tr::Direction::Down).to;
        monotonic = monotonic && to >= prev - 1e-4f;
        prev = to;
    }
    CHECK(monotonic);
    // The shader: a transition() with the brand's colours in it.
    const std::string h = tr::auroraShader(tr::Direction::Up, "#FF0000", "#0000FF");
    CHECK(h.find("float4 transition(float2 uv, float progress)") != std::string::npos && h.find("1.0000, 0.0000, 0.0000") != std::string::npos &&
          h.find("1 - uv.y, uv.x") != std::string::npos);
    CHECK(tr::auroraShader(tr::Direction::Left, "junk", "#12").find("0.2470") != std::string::npos);   // bad colours: the defaults
}

static void testKeying() {
    std::printf("green screen\n");
    namespace kk = ncap::keying;
    kk::Key k;
    k.on = true;
    const kk::Params p = kk::params(k);
    auto px = [&](int r, int g, int b) { return kk::apply(r / 255.0f, g / 255.0f, b / 255.0f, p); };
    // The screen itself, lit brightly and in shadow: clear.
    CHECK(px(0, 177, 64).a < 0.01f && px(20, 210, 90).a < 0.01f && px(0, 110, 40).a < 0.05f);
    // Skin, hair, a red shirt, white, black, a blue jumper: kept.
    CHECK(px(224, 172, 140).a > 0.99f && px(60, 40, 30).a > 0.99f && px(200, 30, 40).a > 0.99f);
    CHECK(px(255, 255, 255).a > 0.99f && px(0, 0, 0).a > 0.99f && px(40, 60, 170).a > 0.99f);
    // Spill: a greenish edge keeps more of itself but loses green.
    const kk::Out edge = px(150, 200, 120);
    CHECK(edge.a > 0.5f && edge.g < 200 / 255.0f && edge.g - edge.r < (200 - 150) / 255.0f);
    // Similarity 0 keeps nearly everything; 100 takes more.
    kk::Key tight = k;
    tight.similarity = 0;
    tight.smoothness = 0;
    CHECK(kk::apply(20 / 255.0f, 210 / 255.0f, 90 / 255.0f, kk::params(tight)).a > 0.5f);
    // A blue screen.
    kk::Key blue = k;
    blue.colour = kk::kBlue;
    const kk::Params pb = kk::params(blue);
    CHECK(kk::apply(0, 71 / 255.0f, 187 / 255.0f, pb).a < 0.01f && kk::apply(0, 177 / 255.0f, 64 / 255.0f, pb).a > 0.99f);
    // Saved and read back; junk takes defaults; values clamped.
    k.similarity = 55;
    k.colour = {10, 200, 30, 255};
    CHECK(kk::fromJson(kk::toJson(k)) == k);
    CHECK(kk::fromJson("nope") == kk::Key{});
    const kk::Key c = kk::fromJson(R"({"on":true,"similarity":500,"colour":"#00FF0080"})");
    CHECK(c.on && c.similarity == 100 && c.colour == ncap::sourceutil::Rgba(0, 255, 0, 255));
}

static void testVoiceEffects() {
    std::printf("voice effects\n");
    using namespace ncap::dsp;
    const float rate = 48000;
    auto sine = [&](float hz, std::size_t frames, float amp = 0.5f) {
        std::vector<float> v(frames * 2);
        for (std::size_t i = 0; i < frames; ++i) v[i * 2] = v[i * 2 + 1] = amp * std::sin(2 * 3.14159265f * hz * (float)i / rate);
        return v;
    };
    // The strongest frequency in the second half (by a plain DFT over a range, every 2 Hz).
    auto peakHz = [&](const std::vector<float>& v, float from, float to) {
        const std::size_t n = v.size() / 2, start = n / 2, len = 8192;
        float best = 0, bestHz = 0;
        for (float hz = from; hz <= to; hz += 2) {
            double re = 0, im = 0;
            for (std::size_t i = 0; i < len; ++i) {
                const double w = 0.5 - 0.5 * std::cos(2 * 3.14159265 * (double)i / len);
                re += v[(start + i) * 2] * w * std::cos(2 * 3.14159265 * hz * (double)i / rate);
                im += v[(start + i) * 2] * w * std::sin(2 * 3.14159265 * hz * (double)i / rate);
            }
            const float m = (float)(re * re + im * im);
            if (m > best) best = m, bestHz = hz;
        }
        return bestHz;
    };
    auto rms = [](const std::vector<float>& v, std::size_t from) {
        double e = 0;
        for (std::size_t i = from * 2; i < v.size(); ++i) e += (double)v[i] * v[i];
        return std::sqrt(e / (double)(v.size() - from * 2));
    };
    CHECK(kTypes == 12 && std::string(key(Type::Reverb)) == "reverb" && std::string(label(Type::Radio)) == "Radio voice");
    Type t;
    CHECK(parse("pitch-shift", t) && t == Type::PitchShift && parse("plugin", t) && t == Type::Plugin);
    // Saved and read back.
    Chain c = {Filter::make(Type::Reverb), Filter::make(Type::PitchShift), Filter::make(Type::Radio)};
    c[1].values["semitones"] = 7;
    const Chain back = fromJson(toJson(c));
    CHECK(back.size() == 3 && back[1].get("semitones") == 7 && back[2].type == Type::Radio && describe(back[1]) == "Pitch shift: +7 semitones");
    // Pitch: an octave up and down, and a fifth.
    for (auto [st, want] : {std::pair<float, float>{12, 880}, {-12, 220}, {7, 659}}) {
        Filter f = Filter::make(Type::PitchShift);
        f.values["semitones"] = st;
        Processor p({f});
        std::vector<float> v = sine(440, 48000);
        p.process(v.data(), 48000);
        const float got = peakHz(v, 150, 1000);
        CHECK(std::fabs(got - want) < 12);
    }
    {   // 0 semitones: untouched
        Filter f = Filter::make(Type::PitchShift);
        f.values["semitones"] = 0;
        Processor p({f});
        std::vector<float> v = sine(440, 4800), w = v;
        p.process(v.data(), 4800);
        CHECK(v == w);
    }
    {   // Reverb: a click leaves a tail that dies away, and it stays finite on long loud input.
        Filter f = Filter::make(Type::Reverb);
        f.values["mix"] = 100;
        Processor p({f});
        std::vector<float> v(48000 * 2 * 3, 0.0f);
        v[0] = v[1] = 1.0f;
        p.process(v.data(), 48000 * 3);
        double early = 0, late = 0;
        for (std::size_t i = 4800 * 2; i < 24000 * 2; ++i) early += (double)v[i] * v[i];
        for (std::size_t i = 120000 * 2; i < 144000 * 2; ++i) late += (double)v[i] * v[i];
        CHECK(early > 1e-4 && late < early * 0.05);
        std::vector<float> loud = sine(200, 48000 * 4, 0.9f);
        p.process(loud.data(), 48000 * 4);
        bool finite = true;
        for (float x : loud) finite = finite && std::isfinite(x) && std::fabs(x) < 4;
        CHECK(finite);
    }
    {   // Radio: 100 Hz and 9 kHz cut hard, 1 kHz kept; static adds noise to silence.
        auto through = [&](float hz, float stat) {
            Filter f = Filter::make(Type::Radio);
            f.values["drive"] = 0;
            f.values["static"] = stat;
            Processor p({f});
            std::vector<float> v = hz > 0 ? sine(hz, 24000, 0.1f) : std::vector<float>(48000, 0.0f);
            p.process(v.data(), v.size() / 2);
            return rms(v, 4800);
        };
        const double mid = through(1000, 0);
        CHECK(20 * std::log10(through(100, 0) / mid) < -15 && 20 * std::log10(through(9000, 0) / mid) < -15);
        CHECK(through(0, 0) < 1e-6 && through(0, 50) > 1e-3);
    }
}

static void testSoundboard() {
    std::printf("soundboard\n");
    namespace sb = ncap::soundboard;
    CHECK(sb::starterPack().size() == 12);
    for (auto& st : sb::starterPack()) {
        const std::vector<float> s = sb::synth(st.id);
        float peak = 0;
        bool finite = true;
        for (float v : s) {
            peak = std::max(peak, std::fabs(v));
            finite = finite && std::isfinite(v);
        }
        const double secs = (double)s.size() / 2 / sb::kRate;
        CHECK(finite && s.size() % 2 == 0 && secs > 0.3 && secs < 3.5);
        CHECK(peak > 0.5f && peak < 0.9f);
        CHECK(std::fabs(s[0]) < 1e-3f && std::fabs(s[s.size() - 1]) < 1e-2f);   // no click at either end
        CHECK(sb::synth(st.id) == s);                                            // the same every time
    }
    CHECK(sb::synth("nope").empty());
    // Voices: several at once, restarting the same one, finishing, stopping.
    auto a = std::make_shared<const std::vector<float>>(std::vector<float>(2 * 100, 0.25f));
    auto b = std::make_shared<const std::vector<float>>(std::vector<float>(2 * 50, 0.5f));
    sb::Voices v;
    v.play(a);
    v.play(b, 0.5f);
    std::vector<float> out(2 * 40, 0.0f);
    v.mix(out.data(), 40);
    CHECK(v.playing() == 2 && std::fabs(out[0] - 0.5f) < 1e-6f);   // 0.25 + 0.5 x 0.5
    std::fill(out.begin(), out.end(), 0.0f);
    v.mix(out.data(), 40);   // b ends after 10 more frames
    CHECK(v.playing() == 1 && std::fabs(out[2 * 9] - 0.5f) < 1e-6f && std::fabs(out[2 * 10] - 0.25f) < 1e-6f);
    v.play(a);   // restart, not a second copy
    CHECK(v.playing() == 1);
    v.play(a, 1.0f, false);
    CHECK(v.playing() == 2);
    v.stopAll();
    CHECK(v.playing() == 0);
    std::fill(out.begin(), out.end(), 0.0f);
    v.mix(out.data(), 40);
    CHECK(out[0] == 0.0f);
}

static void testWizard() {
    std::printf("setup wizard\n");
    namespace wz = ncap::wizard;
    // The bandwidth test: the median after the first 3 seconds; dropped frames take some off.
    wz::TestResult t;
    t.connected = true;
    t.targetKbps = 8000;
    t.kbpsPerSecond = {500, 7000, 9000, 8000, 8100, 7900, 8000, 12000, 7950, 8050};
    CHECK(wz::sustainedKbps(t) == 8000);
    t.droppedFrames = 20;   // 2 a second: 85% - 4% = 81%
    CHECK(wz::sustainedKbps(t) == 6480);
    t.connected = false;
    CHECK(wz::sustainedKbps(t) == 0);
    wz::TestResult brief;
    brief.connected = true;
    brief.kbpsPerSecond = {1, 2, 3};
    CHECK(wz::sustainedKbps(brief) == 0);
    // Hardware encoder and a fast connection: 1080p60 at Twitch's 6000.
    wz::Machine hw{true, 8};
    wz::Plan p = wz::recommend(wz::Goal::Both, hw, 20000);
    CHECK(p.width == 1920 && p.height == 1080 && p.fps == 60 && p.streamKbps == 6000 && p.recordKbps == 40000 && !p.reasons.empty());
    // A slower connection: 75% minus audio, and the picture to suit.
    p = wz::recommend(wz::Goal::Stream, hw, 5000);
    CHECK(p.streamKbps == 3590 && p.width == 1280 && p.fps == 30 && p.recordKbps == 0);
    p = wz::recommend(wz::Goal::Stream, hw, 2000);
    CHECK(p.streamKbps == 1340 && p.width == 1280 && p.height == 720 && p.fps == 30);
    p = wz::recommend(wz::Goal::Stream, hw, 7000);
    CHECK(p.streamKbps == 5090 && p.width == 1280 && p.fps == 60);
    // No hardware encoder: the processor decides.
    p = wz::recommend(wz::Goal::Both, wz::Machine{false, 4}, 20000);
    CHECK(p.width == 1280 && p.fps == 30 && p.streamKbps == 6000);
    p = wz::recommend(wz::Goal::Record, wz::Machine{false, 16}, 0);
    CHECK(p.width == 1920 && p.fps == 30 && p.streamKbps == 0 && p.recordKbps == 20000);
    // Unknown connection: a safe start.
    p = wz::recommend(wz::Goal::Stream, hw, 0);
    CHECK(p.streamKbps == 4500 && p.width == 1280);
    // Recording only with a hardware encoder: generous.
    CHECK(wz::recommend(wz::Goal::Record, hw, 0).recordKbps == 50000);
}

static void testProfileNames() {
    std::printf("profile names\n");
    namespace pf = ncap::profiles;
    std::string why;
    CHECK(pf::validName("Streaming", {"Default"}, why));
    CHECK(!pf::validName("default", {"Default"}, why) && why == "there's one called that already");
    CHECK(!pf::validName("", {}, why) && !pf::validName("   ", {}, why) && !pf::validName("a/b", {}, why) && !pf::validName("x?", {}, why));
    CHECK(!pf::validName("Name.", {}, why) && !pf::validName(" Name", {}, why) && !pf::validName("con", {}, why) && !pf::validName("LPT1.txt", {}, why));
    CHECK(!pf::validName(std::string(41, 'a'), {}, why) && pf::validName(std::string(40, 'a'), {}, why));
    CHECK(pf::validName("Vertical (TikTok) 9:16", {}, why) == false);   // the colon
    CHECK(pf::validName("Vertical - TikTok", {}, why));
    CHECK(pf::uniqueName("Streaming", {"Default"}) == "Streaming" && pf::uniqueName("Streaming", {"streaming", "Streaming 2"}) == "Streaming 3");
    CHECK(pf::recordingName("Art stream", 2026, 11, 15, 14, 0, 5) == "Art stream 2026-11-15 14-00-05");
    CHECK(pf::recordingName("Default", 2026, 1, 2, 3, 4, 5) == "Recording 2026-01-02 03-04-05");
}

// The boxes in an MP4 (type -> payload, first match), walked from the top or inside a box.
static bool mp4Box(const ncap::media::Bytes& b, std::size_t from, std::size_t to, const char* type, std::size_t& at, std::size_t& len) {
    std::size_t p = from;
    while (p + 8 <= to) {
        std::uint64_t sz = ((std::uint32_t)b[p] << 24) | ((std::uint32_t)b[p + 1] << 16) | ((std::uint32_t)b[p + 2] << 8) | b[p + 3];
        std::size_t hdr = 8;
        if (sz == 1) {
            sz = 0;
            for (int i = 0; i < 8; ++i) sz = (sz << 8) | b[p + 8 + i];
            hdr = 16;
        }
        if (sz < hdr || p + sz > to) return false;
        if (std::memcmp(&b[p + 4], type, 4) == 0) {
            at = p + hdr;
            len = (std::size_t)sz - hdr;
            return true;
        }
        p += (std::size_t)sz;
    }
    return false;
}
static std::uint32_t be32(const ncap::media::Bytes& b, std::size_t p) {
    return ((std::uint32_t)b[p] << 24) | ((std::uint32_t)b[p + 1] << 16) | ((std::uint32_t)b[p + 2] << 8) | b[p + 3];
}

static void testMp4() {
    std::printf("mp4 copy\n");
    using namespace ncap;
    media::Bytes sps = {0x67, 0x64, 0x00, 0x28, 0xAC, 0xD9}, pps = {0x68, 0xEB, 0xE3, 0xCB};
    mkv::VideoTrack v;
    v.width = 1920, v.height = 1080, v.fps = 30;
    v.avcC = media::avcC(sps, pps);
    std::vector<mkv::AudioTrack> a = {{"Track 1", 48000, 2, media::aacConfig(48000, 2)}, {"Microphone", 48000, 2, media::aacConfig(48000, 2)}};
    auto frameData = [](int kind, int i) {
        media::Bytes d = {0, 0, 0, 4, (std::uint8_t)(kind == 0 ? (i % 30 == 0 ? 0x65 : 0x41) : 0x21), (std::uint8_t)i, (std::uint8_t)(i >> 8), (std::uint8_t)kind};
        for (int k = 0; k < (i % 7); ++k) d.push_back((std::uint8_t)(k * 13 + kind));
        return d;
    };
    auto makeMkv = [&](bool finish, mkv::MemorySink& sink) {
        mkv::Writer w;
        CHECK(w.begin(&sink, v, a, "NorthRay Capture test"));
        // 3 seconds: video at 30 fps (from 0), audio frames of 1024 samples (track 2 starts 0.5 s late).
        int vi = 0, ai[2] = {0, 0};
        for (std::int64_t us = 0; us < 3000000; us += 1000) {
            if (us >= (std::int64_t)vi * 1000000 / 30) {
                w.video((std::int64_t)vi * 1000000 / 30, vi % 30 == 0, frameData(0, vi));
                ++vi;
            }
            for (int t = 0; t < 2; ++t) {
                const std::int64_t start = t == 1 ? 500000 : 0;
                const std::int64_t at = start + (std::int64_t)ai[t] * 1024 * 1000000 / 48000;
                if (us >= at) {
                    w.audio(t, at, frameData(1 + t, ai[t]));
                    ++ai[t];
                }
            }
        }
        if (finish) CHECK(w.finish());
    };
    mkv::MemorySink full;
    makeMkv(true, full);
    mp4::MemoryInput in(full.bytes);
    mp4::Contents ct;
    std::string err;
    CHECK(mp4::scan(in, ct, err) && ct.tracks.size() == 3 && !ct.cutShort);
    CHECK(ct.tracks[0].video && ct.tracks[0].width == 1920 && ct.tracks[1].name == "Track 1" && ct.tracks[2].name == "Microphone");
    mkv::MemorySink out;
    std::vector<mp4::Chapter> ch = {{0, "Intro"}, {1500000, "Gameplay"}};
    int calls = 0;
    const mp4::Result r = mp4::remux(in, out, ch, [&](double) { ++calls; return true; });
    CHECK(r.ok && r.videoFrames == 90 && r.audioTracks == 2 && calls > 0);
    CHECK(r.durationUs > 2990000 && r.durationUs < 3100000);
    const media::Bytes& m = out.bytes;
    std::size_t at, len, mvAt, mvLen, mdAt, mdLen;
    CHECK(mp4Box(m, 0, m.size(), "ftyp", at, len) && at == 8);
    CHECK(mp4Box(m, 0, m.size(), "moov", mvAt, mvLen) && mp4Box(m, 0, m.size(), "mdat", mdAt, mdLen) && mvAt < mdAt);   // fast start
    // Three tracks; check each one's sizes and offsets point at exactly its frames' bytes.
    std::size_t p = mvAt, tracks = 0;
    std::vector<int> kinds;
    while (true) {
        std::size_t tAt, tLen;
        if (!mp4Box(m, p, mvAt + mvLen, "trak", tAt, tLen)) break;
        ++tracks;
        std::size_t mdia, mdiaL, minf, minfL, stbl, stblL, sz, szL, co, coL, hd, hdL;
        CHECK(mp4Box(m, tAt, tAt + tLen, "mdia", mdia, mdiaL) && mp4Box(m, mdia, mdia + mdiaL, "minf", minf, minfL) &&
              mp4Box(m, minf, minf + minfL, "stbl", stbl, stblL) && mp4Box(m, stbl, stbl + stblL, "stsz", sz, szL) &&
              mp4Box(m, stbl, stbl + stblL, "stco", co, coL) && mp4Box(m, mdia, mdia + mdiaL, "hdlr", hd, hdL));
        const bool video = std::memcmp(&m[hd + 8], "vide", 4) == 0;
        const int kind = video ? 0 : 1 + (int)kinds.size() - 1;
        kinds.push_back(kind);
        const std::uint32_t n = be32(m, sz + 8);
        CHECK(n == (video ? 90u : tracks == 2 ? 141u : 118u));
        bool same = true;
        for (std::uint32_t i = 0; i < n; ++i) {
            const std::uint32_t size = be32(m, sz + 12 + 4 * i), off = be32(m, co + 8 + 4 * i);
            const media::Bytes want = frameData(kind, (int)i);
            same = same && size == want.size() && off + size <= m.size() && std::equal(want.begin(), want.end(), m.begin() + off);
        }
        CHECK(same);
        if (video) {   // keyframes every 30 frames: 1, 31, 61
            std::size_t ss, ssL;
            CHECK(mp4Box(m, stbl, stbl + stblL, "stss", ss, ssL) && be32(m, ss + 4) == 3 && be32(m, ss + 8) == 1 && be32(m, ss + 12) == 31);
            std::size_t tk, tkL;
            CHECK(mp4Box(m, tAt, tAt + tLen, "tkhd", tk, tkL) && (be32(m, tk) & 0xFFFFFF) == 3);
        } else {
            CHECK(std::string(reinterpret_cast<const char*>(&m[hd + 24])) == (tracks == 2 ? "Track 1" : "Microphone"));
            std::size_t tk, tkL, ed, edL;
            CHECK(mp4Box(m, tAt, tAt + tLen, "tkhd", tk, tkL) && (be32(m, tk) & 0xFFFFFF) == (tracks == 2 ? 3u : 2u));   // only the first plays
            CHECK(mp4Box(m, tAt, tAt + tLen, "edts", ed, edL) == (tracks == 3));   // the late track waits 0.5 s
            if (tracks == 3) {
                std::size_t el, elL;
                CHECK(mp4Box(m, ed, ed + edL, "elst", el, elL) && be32(m, el + 4) == 2 && be32(m, el + 8) == 500 && be32(m, el + 12) == 0xFFFFFFFFu);
            }
        }
        p = tAt + tLen;
    }
    CHECK(tracks == 3);
    std::size_t ud, udL, cp, cpL;
    CHECK(mp4Box(m, mvAt, mvAt + mvLen, "udta", ud, udL) && mp4Box(m, ud, ud + udL, "chpl", cp, cpL) && m[cp + 8] == 2);

    // A recording cut short (no index, sizes unknown, the last frame half written) still converts.
    mkv::MemorySink cut;
    makeMkv(false, cut);
    cut.bytes.resize(cut.bytes.size() - 5);
    mp4::MemoryInput cin(cut.bytes);
    mp4::Contents cc;
    CHECK(mp4::scan(cin, cc, err) && cc.cutShort);
    mkv::MemorySink out2;
    const mp4::Result r2 = mp4::remux(cin, out2);
    CHECK(r2.ok && r2.videoFrames >= 89);
    // Not an MKV; cancelled.
    media::Bytes junk = {1, 2, 3, 4, 5, 6, 7, 8};
    mp4::MemoryInput jin(junk);
    mkv::MemorySink out3;
    CHECK(!mp4::remux(jin, out3).ok);
    mkv::MemorySink out4;
    const mp4::Result r4 = mp4::remux(in, out4, {}, [](double) { return false; });
    CHECK(!r4.ok && r4.error == "stopped");
    CHECK(mp4::mp4NameFor("C:\\Videos\\Stream 2026-11-15.MKV") == "C:\\Videos\\Stream 2026-11-15.mp4" && mp4::mp4NameFor("x.dir/file") == "x.dir/file.mp4");
}

static void testDucker() {
    std::printf("auto-duck\n");
    using namespace ncap::dsp;
    DuckSettings st;   // -12 dB, threshold -38, attack 80, hold 400, release 700
    Ducker d;
    // Quiet microphone: nothing changes.
    for (int i = 0; i < 50; ++i) {
        auto g = d.block(dbToLinear(-60), 10, st);
        CHECK(g.first == 1.0f && g.second == 1.0f);
    }
    // Talking: down to -12 dB within the attack time, smoothly (never a jump bigger than one step).
    float prev = 1;
    int blocks = 0;
    while (d.gainDb() > -11.99f && blocks < 100) {
        auto g = d.block(dbToLinear(-20), 10, st);
        CHECK(std::fabs(g.first - prev) < 1e-6f);   // each block starts where the last ended
        CHECK(linearToDb(g.first) - linearToDb(g.second) <= 12.0f * 10 / 80 + 0.01f);
        prev = g.second;
        ++blocks;
    }
    CHECK(blocks >= 7 && blocks <= 9 && d.ducking());   // 80 ms in 10 ms blocks
    // Stops talking: holds for 400 ms, then comes back over 700 ms.
    for (int i = 0; i < 39; ++i) d.block(dbToLinear(-60), 10, st);
    CHECK(d.gainDb() < -11.9f);
    blocks = 0;
    while (d.gainDb() < -0.01f && blocks < 200) {
        d.block(dbToLinear(-60), 10, st);
        ++blocks;
    }
    CHECK(blocks >= 70 && blocks <= 73 && !d.ducking());
    auto g = d.block(0, 10, st);
    CHECK(g.second == 1.0f);   // back to exactly unchanged
    // A short word mid-release ducks again straight away.
    for (int i = 0; i < 5; ++i) d.block(dbToLinear(-10), 10, st);
    CHECK(d.gainDb() < -5);
    // Amount 0 dB: never changes anything.
    Ducker z;
    DuckSettings none;
    none.amountDb = 0;
    for (int i = 0; i < 20; ++i) CHECK(z.block(1.0f, 10, none).second == 1.0f);
}

static void testChat() {
    std::printf("twitch chat\n");
    namespace ch = ncap::chat;
    ch::IrcLine l;
    // A real-shaped PRIVMSG with tags (escaped values), badges, colour and a /me action.
    CHECK(ch::parseLine("@badge-info=;badges=moderator/1,subscriber/12;color=#1E90FF;display-name=Cj\\sTest;id=abc-1;user-id=42 "
                        ":cjtest!cjtest@cjtest.tmi.twitch.tv PRIVMSG #cjphilip :\x01" "ACTION waves hello\x01\r\n", l));
    CHECK(l.command == "PRIVMSG" && l.params.size() == 2 && l.params[0] == "#cjphilip" && l.tags["display-name"] == "Cj Test");
    ch::Event e = ch::interpret(l, 1000);
    CHECK(e.type == ch::Event::Chat && e.message.login == "cjtest" && e.message.name == "Cj Test" && e.message.id == "abc-1");
    CHECK(e.message.userId == "42" && e.message.action && e.message.text == "waves hello");
    CHECK(e.message.colour == ncap::sourceutil::Rgba(0x1E, 0x90, 0xFF, 255));
    CHECK(e.message.badges.size() == 2 && e.message.badges[0] == "moderator" && e.message.badges[1] == "subscriber");
    CHECK(ch::unescapeTag("a\\:b\\\\c\\sd\\") == "a;b\\c d");
    // No colour, no display name; control characters cleaned.
    CHECK(ch::parseLine(":bob!bob@bob PRIVMSG #c :hi\tthere", l));
    e = ch::interpret(l, 2000);
    CHECK(e.type == ch::Event::Chat && e.message.name == "bob" && e.message.colour.a == 0 && e.message.text == "hi there");
    // Ping, reconnect, notice, broken lines.
    CHECK(ch::parseLine("PING :tmi.twitch.tv", l) && ch::interpret(l, 0).type == ch::Event::Ping && ch::interpret(l, 0).arg == "tmi.twitch.tv");
    CHECK(ch::parseLine(":tmi.twitch.tv RECONNECT", l) && ch::interpret(l, 0).type == ch::Event::Reconnect);
    CHECK(!ch::parseLine("", l) && !ch::parseLine("@a=b ", l));
    CHECK(ch::parseLine(":tmi.twitch.tv 001 justinfan123 :Welcome", l) && ch::interpret(l, 0).type == ch::Event::None);

    // The log: deletions, timeouts and bans, clears, and keeping only the newest.
    ch::Log log(3);
    auto msg = [&](const char* id, const char* uid, const char* text, std::int64_t at) {
        ch::Event x;
        x.type = ch::Event::Chat;
        x.message.id = id;
        x.message.userId = uid;
        x.message.login = std::string("u") + uid;
        x.message.text = text;
        x.message.atMs = at;
        return x;
    };
    CHECK(log.apply(msg("1", "7", "one", 0)) && log.apply(msg("2", "8", "two", 1000)) && log.apply(msg("3", "7", "three", 2000)));
    const unsigned v = log.version();
    CHECK(ch::parseLine("@login=u8;target-msg-id=2 :tmi.twitch.tv CLEARMSG #c :two", l) && log.apply(ch::interpret(l, 0)));
    CHECK(log.messages().size() == 2 && log.version() > v);
    CHECK(ch::parseLine("@target-msg-id=nope :tmi.twitch.tv CLEARMSG #c :x", l) && !log.apply(ch::interpret(l, 0)));
    CHECK(ch::parseLine("@ban-duration=600;target-user-id=7 :tmi.twitch.tv CLEARCHAT #c :u7", l) && log.apply(ch::interpret(l, 0)));
    CHECK(log.messages().empty());
    for (int i = 0; i < 5; ++i) log.apply(msg(std::to_string(i).c_str(), "9", "m", i * 1000));
    CHECK(log.messages().size() == 3 && log.messages().front().id == "2" && log.messages().back().id == "4");
    CHECK(ch::parseLine(":tmi.twitch.tv CLEARCHAT #c", l) && ch::interpret(l, 0).type == ch::Event::ClearAll && log.apply(ch::interpret(l, 0)));
    CHECK(log.messages().empty());

    // What's shown: the newest few, and fading.
    std::vector<ch::Message> all;
    for (int i = 0; i < 6; ++i) all.push_back(msg(std::to_string(i).c_str(), "1", "m", i * 10000).message);
    auto shown = ch::recent(all, 4, 0, 60000);
    CHECK(shown.size() == 4 && shown.front().id == "2" && shown.back().id == "5");
    shown = ch::recent(all, 4, 25, 60000);   // only the last 25 s: at 40 s and 50 s
    CHECK(shown.size() == 2 && shown.front().id == "4");
    CHECK(ch::recent(all, 4, 5, 60000).empty());

    // Channel names from what people type.
    CHECK(ch::channelFrom("CJPhilip") == "cjphilip" && ch::channelFrom("@cjphilip") == "cjphilip");
    CHECK(ch::channelFrom(" https://www.twitch.tv/CJPhilip/ ") == "cjphilip" && ch::channelFrom("twitch.tv/cj_philip?x=1") == "cj_philip");
    CHECK(ch::channelFrom("").empty() && ch::channelFrom("not a name").empty() && ch::channelFrom("youtube.com/@cj").empty());
    const auto lines = ch::loginLines("cjphilip", 123);
    CHECK(lines.size() == 4 && lines[1] == "PASS SCHMOOPIIE" && lines[2] == "NICK justinfan10123" && lines[3] == "JOIN #cjphilip");
    CHECK(ch::defaultColour("abc") == ch::defaultColour("abc") && ch::defaultColour("abc").a == 255);
    for (auto& m : ch::samples(0)) CHECK(m.name.rfind("Sample", 0) == 0);

    // The chat element in designs: saved, read back, follows the brand, counts as live.
    namespace dz = ncap::design;
    dz::Document d;
    dz::Element c;
    c.kind = dz::Kind::Chat;
    c.chatCount = 12;
    c.chatFade = 30;
    c.chatBubbles = false;
    c.chatNameColours = false;
    const std::string id = dz::add(d, c);
    CHECK(dz::usesChat(d) && dz::usesTime(d));
    const dz::Document back = dz::fromJson(dz::toJson(d));
    const dz::Element* b = back.find(id);
    CHECK(b && b->kind == dz::Kind::Chat && b->chatCount == 12 && b->chatFade == 30 && !b->chatBubbles && !b->chatNameColours && b->chatBubbleBrand == "panel");
    dz::Brand k = dz::defaultBrand();
    k.colours[4].colour = {1, 2, 3, 255};
    CHECK(dz::applyBrand(back, k).find(id)->chatBubble == ncap::sourceutil::Rgba(1, 2, 3, 210));
    bool tpl = false;
    for (auto& tp : dz::templates()) tpl = tpl || dz::usesChat(tp.doc);
    CHECK(tpl);
}

static void testBrandKit() {
    std::printf("brand kit\n");
    namespace dz = ncap::design;
    const dz::Brand def = dz::defaultBrand();
    CHECK(def.colours.size() == dz::brandKeys().size() && dz::brandColour(def, "primary") && !dz::brandColour(def, "nope"));
    // Round trip, and bad parts take the defaults.
    dz::Brand b = def;
    b.colours[0].colour = {255, 0, 0, 255};
    b.headingFont = "Georgia";
    b.fontFiles = {"C:\\Fonts\\Mine.ttf"};
    b.logo = "C:\\logo.png";
    CHECK(dz::brandFromJson(dz::toJson(b)) == b);
    const dz::Brand bad = dz::brandFromJson(R"({"colours":{"primary":"#00FF0080","text":"pink"},"headingFont":"brand:body","fontFiles":[1,""]})");
    CHECK(bad.colours[0].colour == ncap::sourceutil::Rgba(0, 255, 0, 255));   // made solid
    CHECK(bad.colours[2].colour == def.colours[2].colour && bad.headingFont == "Segoe UI" && bad.fontFiles.empty());
    CHECK(dz::brandFromJson("not json") == def);

    // Linking: matching colours (any see-through) and Segoe UI text.
    dz::Document d;
    dz::Element r;
    r.kind = dz::Kind::Rect;
    r.fill = dz::Fill::linear({{0, {0x3F, 0xCF, 0x8E, 0x55}}, {1, {1, 2, 3, 255}}}, 0);
    r.stroke = {0x7A, 0x5C, 0xFF, 255};
    r.shadow.colour = {0, 0, 0, 140};
    const std::string rid = dz::add(d, r);
    dz::Element t;
    t.kind = dz::Kind::Text;
    t.bold = true;
    t.fill = dz::Fill::solid({0xF4, 0xF7, 0xFB, 255});
    const std::string tid = dz::add(d, t);
    dz::Element p;
    p.kind = dz::Kind::Image;
    p.file = dz::kBrandLogo;
    const std::string pid = dz::add(d, p);
    CHECK(dz::usesBrand(d));   // the logo
    dz::linkToBrand(d, def);
    CHECK(d.find(rid)->fill.stops[0].brand == "primary" && d.find(rid)->fill.stops[1].brand.empty());
    CHECK(d.find(rid)->strokeBrand == "secondary" && d.find(rid)->shadow.brand.empty());
    CHECK(d.find(tid)->fill.brand == "text" && d.find(tid)->font == dz::kBrandHeading);
    // Saved and read back with the links.
    const dz::Document back = dz::fromJson(dz::toJson(d));
    CHECK(back.find(rid)->fill.stops[0].brand == "primary" && back.find(rid)->strokeBrand == "secondary" && back.find(tid)->fill.brand == "text");

    // Applying another kit: the colour changes, the see-through stays.
    const dz::Document a = dz::applyBrand(d, b);
    CHECK(a.find(rid)->fill.stops[0].colour == ncap::sourceutil::Rgba(255, 0, 0, 0x55));
    CHECK(a.find(rid)->fill.stops[1].colour == ncap::sourceutil::Rgba(1, 2, 3, 255));
    CHECK(a.find(tid)->font == "Georgia" && a.find(pid)->file == "C:\\logo.png");
    CHECK(a.find(rid)->stroke == def.colours[1].colour);
    // Unlinking keeps what's shown.
    const dz::Document u = dz::unlinkBrand(d, b);
    CHECK(!dz::usesBrand(u) && u.find(rid)->fill.stops[0].colour == ncap::sourceutil::Rgba(255, 0, 0, 0x55) && u.find(tid)->font == "Georgia");

    // Every template follows the kit, and the watermark uses the logo.
    for (auto& tp : dz::templates()) CHECK(dz::usesBrand(tp.doc));
    bool logo = false;
    for (auto& tp : dz::templates())
        for (auto& e : tp.doc.elements) logo = logo || e.file == dz::kBrandLogo;
    CHECK(logo);
}

static void testDesignLive() {
    std::printf("design timers, QR codes, icons\n");
    using namespace ncap::design;
    Element t;
    t.kind = Kind::Timer;
    t.timerMode = "countdown";
    t.timerSeconds = 300;
    Clock c;
    CHECK(timerText(t, c).text == "5:00" && !timerText(t, c).ended);
    c.elapsedMs = 200;
    CHECK(timerText(t, c).text == "5:00");          // 4:59.8 left still reads 5:00
    c.elapsedMs = 1000;
    CHECK(timerText(t, c).text == "4:59");
    c.elapsedMs = 299500;
    TimerShown s = timerText(t, c);
    CHECK(s.text == "0:01" && s.pulse > 1 && s.remainingMs == 500);
    c.elapsedMs = 290000;
    CHECK(timerText(t, c).pulse == 1 || timerText(t, c).pulse > 1);   // exactly 10 s left: at the edge
    c.elapsedMs = 280000;
    CHECK(timerText(t, c).pulse == 1);              // 20 s left: no pulse yet
    c.elapsedMs = 300000;
    s = timerText(t, c);
    CHECK(s.ended && s.text == "Starting now!" && s.remainingMs == 0);
    t.timerPulse = false;
    c.elapsedMs = 299500;
    CHECK(timerText(t, c).pulse == 1);
    t.text = "Starting in {time}";
    CHECK(timerText(t, c).text == "Starting in 0:01");
    t.text = "";
    t.timerSeconds = 3 * 3600 + 5;
    c.elapsedMs = 0;
    CHECK(timerText(t, c).text == "3:00:05");

    // Until a time of day: before it, after it (ended), and long after (tomorrow's).
    t.timerMode = "until";
    t.timerUntil = 20 * 60;   // 20:00
    c.localSeconds = 19 * 3600 + 58 * 60 + 30;
    CHECK(timerText(t, c).text == "1:30");
    c.localSeconds = 20 * 3600 + 5;
    CHECK(timerText(t, c).ended);
    t.timerUntil = 6 * 60;    // 06:00, asked at 20:00: tomorrow morning, 10 hours away
    CHECK(timerText(t, c).text == "9:59:55");

    t.timerMode = "stopwatch";
    c.elapsedMs = 61000;
    CHECK(timerText(t, c).text == "1:01");
    t.timerMode = "live";
    CHECK(timerText(t, c).text == "Not live");
    c.liveMs = 3723000;
    CHECK(timerText(t, c).text == "1:02:03");
    t.timerMode = "clock";
    c.localSeconds = 9 * 3600 + 5 * 60 + 59;
    c.dateText = "Sun 15 Nov";
    CHECK(timerText(t, c).text == "09:05");
    t.timerDate = true;
    CHECK(timerText(t, c).text == "09:05\nSun 15 Nov");

    // Saved and read back; usesTime.
    Document d;
    t.timerScene = "s7";
    const std::string id = add(d, t);
    CHECK(d.find(id)->name == "Timer" && usesTime(d));
    const Document back = fromJson(toJson(d));
    CHECK(back.elements == d.elements);
    d.find(id)->visible = false;
    CHECK(!usesTime(d));
    CHECK(fromJson(R"({"elements":[{"kind":"timer","id":"e1","timer":{"mode":"bogus","seconds":-5}}]})").elements[0].timerMode == "countdown");

    // QR codes: square, odd-sized, with the three finder patterns in their corners.
    const auto q = qrModules("https://northray.example/download");
    CHECK(!q.empty() && q.size() == q[0].size() && q.size() % 4 == 1 && q.size() >= 21);
    const std::size_t n = q.size();
    auto finder = [&](std::size_t x0, std::size_t y0) {
        for (std::size_t i = 0; i < 7; ++i)
            if (!q[y0][x0 + i] || !q[y0 + 6][x0 + i] || !q[y0 + i][x0] || !q[y0 + i][x0 + 6]) return false;   // the dark ring
        return !q[y0 + 1][x0 + 1] && q[y0 + 3][x0 + 3];   // light inside it, dark centre
    };
    CHECK(finder(0, 0) && finder(n - 7, 0) && finder(0, n - 7));
    CHECK(qrModules("").empty() && qrModules(std::string(2000, 'x')).empty());
    CHECK(qrModules("a") != q);

    // Icons: each parses to closed figures within its 24 x 24 box.
    CHECK(icons().size() >= 16 && iconById("star") && !iconById("nope"));
    for (auto& ic : icons()) {
        const auto figs = iconFigures(ic.path);
        CHECK(!figs.empty());
        for (auto& f : figs) {
            CHECK(f.size() >= 3);
            for (auto& [x, y] : f) CHECK(x >= -0.01f && x <= 24.01f && y >= -0.01f && y <= 24.01f);
        }
    }
    CHECK(iconFigures("M 1 1 L 2").size() == 1);   // stops at bad input, keeps what it had
}

static void testDsp() {
    std::printf("audio filters\n");
    namespace d = ncap::dsp;
    using namespace dsptest;
    auto db = [](float a, float b) { return d::linearToDb(a / b); };
    // Gain.
    {
        d::Filter f = d::Filter::make(d::Type::Gain);
        f.values["gain"] = 6;
        d::Processor p({f});
        auto v = sine(1000, 0.1f, 4800);
        run(p, v);
        CHECK(std::fabs(db(rms(v, 0, 4800), 0.1f / std::sqrt(2.0f)) - 6) < 0.05f);
    }
    // Noise gate: closes on quiet sound, opens on loud.
    {
        d::Processor p({d::Filter::make(d::Type::NoiseGate)});
        auto quiet = sine(500, 0.003f, 48000);   // about -50 dB
        run(p, quiet);
        CHECK(rms(quiet, 24000, 48000) < 0.0002f);
        auto loud = sine(500, 0.3f, 48000);
        run(p, loud);
        CHECK(std::fabs(db(rms(loud, 4800, 48000), 0.3f / std::sqrt(2.0f))) < 0.5f);
    }
    // Compressor: 12 dB over the threshold at 4:1 comes out about 9 dB lower.
    {
        d::Filter f = d::Filter::make(d::Type::Compressor);
        f.values["threshold"] = -24;
        f.values["ratio"] = 4;
        d::Processor p({f});
        auto v = sine(1000, d::dbToLinear(-12), 48000);
        run(p, v);
        const float outPeakDb = d::linearToDb(rms(v, 24000, 48000) * std::sqrt(2.0f));
        CHECK(outPeakDb < -19.5f && outPeakDb > -22.5f);
        CHECK(p.reductionDb() < -7);
        auto under = sine(1000, d::dbToLinear(-30), 48000);
        d::Processor p2({f});
        run(p2, under);
        CHECK(std::fabs(d::linearToDb(rms(under, 24000, 48000) * std::sqrt(2.0f)) + 30) < 0.3f);
    }
    // Expander: quieter below the threshold, untouched above.
    {
        d::Filter f = d::Filter::make(d::Type::Expander);
        f.values["threshold"] = -40;
        f.values["ratio"] = 2;
        d::Processor p({f});
        auto v = sine(800, d::dbToLinear(-50), 48000);
        run(p, v);
        const float out = d::linearToDb(rms(v, 24000, 48000) * std::sqrt(2.0f));
        CHECK(out < -56 && out > -64);
    }
    // Limiter: nothing ever over the ceiling, even a sudden full-scale burst.
    {
        d::Filter f = d::Filter::make(d::Type::Limiter);
        f.values["ceiling"] = -6;
        d::Processor p({f});
        auto v = sine(200, 0.05f, 9600);
        auto burst = sine(3000, 1.0f, 9600);
        v.insert(v.end(), burst.begin(), burst.end());
        run(p, v);
        float peak = 0;
        for (float x : v) peak = std::max(peak, std::fabs(x));
        CHECK(peak <= d::dbToLinear(-6) + 1e-5f);
        CHECK(p.latencyFrames() == 72);
    }
    // Equaliser: +6 dB low shelf lifts 50 Hz by about 6 dB, leaves 5 kHz alone.
    {
        d::Filter f = d::Filter::make(d::Type::Equaliser);
        f.values["low"] = 6;
        d::Processor p({f});
        auto lo = sine(50, 0.1f, 96000);
        run(p, lo);
        CHECK(std::fabs(db(rms(lo, 48000, 96000), 0.1f / std::sqrt(2.0f)) - 6) < 0.6f);
        d::Processor p2({f});
        auto hi = sine(5000, 0.1f, 48000);
        run(p2, hi);
        CHECK(std::fabs(db(rms(hi, 24000, 48000), 0.1f / std::sqrt(2.0f))) < 0.3f);
        d::Filter m = d::Filter::make(d::Type::Equaliser);
        m.values["mid"] = -12;
        m.values["midFreq"] = 1000;
        d::Processor p3({m});
        auto mid = sine(1000, 0.1f, 48000);
        run(p3, mid);
        CHECK(std::fabs(db(rms(mid, 24000, 48000), 0.1f / std::sqrt(2.0f)) + 12) < 0.6f);
    }
    // Balance: full left silences the right; centre changes nothing.
    {
        d::Filter f = d::Filter::make(d::Type::Balance);
        f.values["balance"] = -100;
        d::Processor p({f});
        auto v = sine(440, 0.5f, 4800);
        run(p, v);
        CHECK(rms(v, 0, 4800, 1) < 1e-4f && rms(v, 0, 4800, 0) > 0.3f);
        d::Processor p2({d::Filter::make(d::Type::Balance)});
        auto c = sine(440, 0.5f, 4800);
        run(p2, c);
        CHECK(std::fabs(rms(c, 0, 4800, 1) - rms(c, 0, 4800, 0)) < 1e-5f);
    }
    // Delay: an impulse comes out exactly the delay later.
    {
        d::Filter f = d::Filter::make(d::Type::Delay);
        f.values["delay"] = 10;   // 480 frames
        d::Processor p({f});
        std::vector<float> v(4800 * 2, 0.0f);
        v[0] = v[1] = 1;
        run(p, v);
        CHECK(v[480 * 2] == 1 && v[480 * 2 + 1] == 1 && v[0] == 0 && p.latencyFrames() == 480);
    }
    // Noise suppression: steady noise made much quieter; a tone in it kept (and nothing changed when there's no noise).
    {
        d::Processor p({d::Filter::make(d::Type::NoiseSuppression)});
        auto n = noise(0.05f, 48000 * 3);
        const float before = rms(n, 48000 * 2, 48000 * 3);
        run(p, n);
        const float after = rms(n, 48000 * 2, 48000 * 3);
        CHECK(db(after, before) < -10);
        d::Processor p2({d::Filter::make(d::Type::NoiseSuppression)});
        // Speech-like: a tone in bursts (0.25 s on, 0.25 s off) over steady noise. The bursts keep their level.
        auto mix = noise(0.02f, 48000 * 3, 7);
        auto tone = sine(1000, 0.3f, 48000 * 3);
        for (std::size_t i = 0; i < mix.size() / 2; ++i)
            if ((i / 12000) % 2 == 0) {
                mix[i * 2] += tone[i * 2];
                mix[i * 2 + 1] += tone[i * 2 + 1];
            }
        run(p2, mix);
        // The last burst (2.5 s to 2.75 s, shifted by the 512-frame latency), away from its edges.
        const std::size_t from = 120000 + 512 + 2400, to = 132000 + 512 - 2400;
        CHECK(std::fabs(db(rms(mix, from, to), 0.3f / std::sqrt(2.0f))) < 1.5f);
        // And between bursts, the noise is pushed down.
        CHECK(rms(mix, 132000 + 512 + 4800, 144000) < 0.02f / std::sqrt(3.0f) * 0.4f);
        CHECK(p2.latencyFrames() == 512);
    }
    // The chain as JSON: order, switches and values kept; junk dropped; values clamped.
    {
        d::Chain c = {d::Filter::make(d::Type::NoiseGate), d::Filter::make(d::Type::Compressor), d::Filter::make(d::Type::Limiter)};
        c[1].enabled = false;
        c[1].values["ratio"] = 8;
        const d::Chain r = d::fromJson(d::toJson(c));
        CHECK(r.size() == 3 && r[0].type == d::Type::NoiseGate && !r[1].enabled && r[1].get("ratio") == 8 && r[2].type == d::Type::Limiter);
        const d::Chain bad = d::fromJson(R"([{"type":"echo-chamber"},{"type":"gain","values":{"gain":99}},5])");
        CHECK(bad.size() == 1 && bad[0].get("gain") == 30);
        CHECK(d::describe(c[1]) == "Compressor: above -18 dB, 8.0:1");
        CHECK(d::Processor(c).latencyFrames() == 72);   // the disabled compressor isn't run
    }
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
    testSceneFiles();
    testGroups();
    testDsp();
    testBorder();
    testDesign();
    testDesignLive();
    testBrandKit();
    testChat();
    testTrackNames();
    testDucker();
    testMp4();
    testProfileNames();
    testWizard();
    testSoundboard();
    testVoiceEffects();
    testKeying();
    testAuroraSweep();
    testGif();
    testChecklist();
    testAutomation();
    testPlugins();
    std::printf("\n%d passed, %d failed\n", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

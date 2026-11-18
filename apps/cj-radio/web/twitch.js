// Runs inside CJ Radio's embedded browser. Shows Twitch's player for the
// channel CJ Radio asked for, and tells CJ Radio what the player reports.
(function () {
  "use strict";
  var host = window.chrome && window.chrome.webview;
  function tell(msg) { if (host) host.postMessage(msg); }

  var channel = new URLSearchParams(location.search).get("channel") || "";
  if (!/^[a-z0-9_]{1,25}$/.test(channel)) {
    document.getElementById("note").style.display = "flex";
    tell("failed");
    return;
  }
  // Twitch's script didn't load: Twitch couldn't be reached (no connection, or Twitch is down).
  if (typeof Twitch === "undefined" || !Twitch.Player) {
    document.getElementById("note").style.display = "flex";
    tell("unreachable");
    return;
  }

  var player = new Twitch.Player("player", {
    channel: channel,
    width: "100%",
    height: "100%",
    parent: [location.hostname],
    autoplay: false,   // the listener chooses when to start
    muted: false
  });

  var readyTold = false;
  player.addEventListener(Twitch.Player.READY, function () { readyTold = true; tell("ready"); });
  // The player never got going (its own requests to Twitch failing): say so rather than wait for ever.
  setTimeout(function () { if (!readyTold) tell("unreachable"); }, 25000);
  // ONLINE / OFFLINE come from Twitch itself: whether the channel is broadcasting.
  player.addEventListener(Twitch.Player.ONLINE, function () { tell("online"); });
  player.addEventListener(Twitch.Player.OFFLINE, function () { tell("offline"); });
  player.addEventListener(Twitch.Player.ENDED, function () { tell("offline"); });
  // Sound only uses Twitch's own "Audio Only" quality, when the stream offers it.
  // The player stays on screen either way.
  var soundOnly = new URLSearchParams(location.search).get("sound") === "only";
  function qualities() { try { return player.getQualities() || []; } catch (e) { return []; } }
  function find(test) {
    var list = qualities();
    for (var i = 0; i < list.length; i++) {
      var q = list[i], id = String(q.group || q.name || "").toLowerCase();
      if (test(id, String(q.name || "").toLowerCase())) return q.group || q.name;
    }
    return null;
  }
  var switchedToAudio = false;
  function applySound() {
    if (soundOnly) {
      var audio = find(function (id, name) { return id.indexOf("audio") === 0 || name.indexOf("audio") === 0; });
      if (audio) { player.setQuality(audio); switchedToAudio = true; tell("audio-on"); }
      else tell("audio-unavailable");
    } else {
      // Only undo our own change; leave a quality the listener picked alone.
      if (switchedToAudio) {
        var auto = find(function (id) { return id === "auto"; }) || find(function (id) { return id === "chunked"; });
        if (auto) player.setQuality(auto);
        switchedToAudio = false;
      }
      tell("audio-off");
    }
  }

  // When a show starts playing, its sound fades in over about two seconds instead of starting at full
  // volume (once per show: after the channel goes offline and back on, it fades in again).
  var fadedIn = false, fadeTimer = null;
  function fadeIn() {
    if (fadedIn) return;
    fadedIn = true;
    var target;
    try { target = player.getVolume(); } catch (e) { return; }
    if (!(target > 0)) return;
    var step = 0, steps = 30;
    try { player.setVolume(0); } catch (e) { return; }
    clearInterval(fadeTimer);
    fadeTimer = setInterval(function () {
      step++;
      var t = step / steps;
      try { player.setVolume(target * t * t); } catch (e) { clearInterval(fadeTimer); return; }
      if (step >= steps) clearInterval(fadeTimer);
    }, 65);
  }
  player.addEventListener(Twitch.Player.OFFLINE, function () { fadedIn = false; });
  player.addEventListener(Twitch.Player.PLAYING, function () { tell("playing"); applySound(); fadeIn(); });
  player.addEventListener(Twitch.Player.PAUSE, function () { tell("paused"); });

  // CJ Radio pauses the player when the Listen page isn't showing.
  if (host) {
    host.addEventListener("message", function (e) {
      if (e.data === "pause") { try { player.pause(); } catch (err) { /* not started yet */ } }
      else if (e.data === "play") { try { player.play(); } catch (err) { /* not started yet */ } }
      else if (e.data === "audio" || e.data === "video") {
        soundOnly = e.data === "audio";
        // Qualities are only known once the stream is playing; PLAYING applies it then.
        if (qualities().length) applySound(); else tell(soundOnly ? "audio-on" : "audio-off");
      }
    });
  }
})();

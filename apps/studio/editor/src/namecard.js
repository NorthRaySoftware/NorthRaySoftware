// NorthRay Writing Studio: the name card. Hovering a name from the world's bible
// (a dotted underline in the text) shows who or what it is: the picture, name,
// kind, other names, summary and the first few details, with a way to open it.

const OBJECTS = "https://objects.nrstudio.localhost/";
let entries = new Map();   // id -> {id, name, kind, names, summary, details, image}
let send = () => {};
let timer = null, hideTimer = null, showingFor = null;
const $ = (id) => document.getElementById(id);

export function setEntries(list) {
  entries = new Map();
  for (const e of Array.isArray(list) ? list : []) if (e && e.id) entries.set(e.id, e);
}

function initials(name) {
  const parts = String(name || "?").trim().split(/\s+/).filter(Boolean);
  return ((parts[0] || "?")[0] + (parts.length > 1 ? parts[parts.length - 1][0] : "")).toUpperCase();
}

function show(target) {
  const e = entries.get(target.dataset.entry);
  if (!e) return;
  const card = $("namecard");
  card.replaceChildren();
  const top = document.createElement("div");
  top.className = "nc-top";
  const pic = document.createElement("div");
  pic.className = "nc-pic";
  if (/^[0-9a-f]{64}$/.test(e.image || "")) {
    const img = document.createElement("img");
    img.src = OBJECTS + e.image.slice(0, 2) + "/" + e.image.slice(2);
    img.alt = "";
    img.addEventListener("error", () => { pic.textContent = initials(e.name); pic.classList.add("nc-initials"); });
    pic.append(img);
  } else {
    pic.textContent = initials(e.name);
    pic.classList.add("nc-initials");
  }
  const head = document.createElement("div");
  head.className = "nc-head";
  const kind = document.createElement("div");
  kind.className = "nc-kind";
  kind.textContent = e.kind || "";
  const name = document.createElement("div");
  name.className = "nc-name";
  name.textContent = e.name || "";
  head.append(kind, name);
  const others = (e.names || []).filter((n) => n && n !== e.name);
  if (others.length) {
    const aka = document.createElement("div");
    aka.className = "nc-aka";
    aka.textContent = "Also: " + others.join(", ");
    head.append(aka);
  }
  top.append(pic, head);
  card.append(top);
  if (e.summary) {
    const s = document.createElement("div");
    s.className = "nc-summary";
    s.textContent = e.summary;
    card.append(s);
  }
  const lines = String(e.details || "").split("\n").map((l) => l.trim()).filter(Boolean);
  if (lines.length) {
    const t = document.createElement("dl");
    t.className = "nc-details";
    for (const l of lines) {
      const i = l.indexOf(":");
      const dt = document.createElement("dt"), dd = document.createElement("dd");
      if (i > 0) { dt.textContent = l.slice(0, i).trim(); dd.textContent = l.slice(i + 1).trim(); }
      else { dt.textContent = ""; dd.textContent = l; }
      t.append(dt, dd);
    }
    card.append(t);
  }
  const open = document.createElement("button");
  open.className = "nc-open";
  open.textContent = "Open in Worldbuilding";
  open.addEventListener("click", () => { hide(true); send({ type: "entry", id: e.id }); });
  card.append(open);
  card.hidden = false;
  const r = target.getBoundingClientRect();
  const w = Math.min(340, window.innerWidth - 16);
  card.style.width = w + "px";
  card.style.left = Math.max(8, Math.min(r.left, window.innerWidth - w - 8)) + "px";
  const h = card.offsetHeight, below = r.bottom + 8;
  card.style.top = (below + h < window.innerHeight - 8 ? below : Math.max(8, r.top - h - 8)) + "px";
  showingFor = target;
}

function hide(now) {
  clearTimeout(timer);
  clearTimeout(hideTimer);
  const go = () => { $("namecard").hidden = true; showingFor = null; };
  if (now) go(); else hideTimer = setTimeout(go, 250);
}

export function initNameCard(sender) {
  send = sender;
  document.addEventListener("mouseover", (ev) => {
    if (ev.target.closest && ev.target.closest("#namecard")) { clearTimeout(hideTimer); return; }
    // A style hint on the same words has its own card.
    const t = ev.target.closest && !ev.target.closest(".hint") ? ev.target.closest(".wl[data-entry]") : null;
    if (t) {
      clearTimeout(hideTimer);
      if (t === showingFor) return;
      clearTimeout(timer);
      timer = setTimeout(() => show(t), 400);
    } else if (showingFor || !$("namecard").hidden) hide(false);
    else clearTimeout(timer);
  });
  document.addEventListener("keydown", () => hide(true), true);
  $("scroller").addEventListener("scroll", () => hide(true));
}

// NorthRay Writing Studio: the ebook preview. One chapter at a time is flowed
// into CSS columns the size of the device's screen; turning a page moves the
// columns along. Messages from the app:
//   {type:"load", chapters:[{title, part, html}], css, title, language, fonts:{family, regular, italic, bold, boldItalic}, chapter}
//   {type:"settings", device, theme, scale, font, dark}   {type:"goto", chapter}
// To the app: {type:"ready"} {type:"position", chapter, page, pages}
(() => {
  const host = window.chrome && window.chrome.webview;
  const send = (m) => { if (host) host.postMessage(m); else window.dispatchEvent(new CustomEvent("studio-out", { detail: m })); };
  const $ = (id) => document.getElementById(id);
  const device = $("device"), screen = $("screen"), reading = $("reading"), flow = $("flow");
  let chapters = [], current = 0, page = 0, pages = 1, fontCss = "";
  let settings = { device: "ereader", theme: "white", scale: 1, font: "book", dark: false };
  const bookStyle = document.createElement("style");
  const fontStyle = document.createElement("style");
  document.head.append(bookStyle, fontStyle);

  function fitDevice() {
    const w = device.offsetWidth, h = device.offsetHeight;
    const s = Math.min((window.innerWidth - 40) / w, (window.innerHeight - 40) / h, 1.2);
    device.style.transform = "scale(" + s + ")";
  }

  function geometry() {
    const cs = getComputedStyle(device);
    const margin = parseFloat(cs.getPropertyValue("--margin")) || 30;
    const top = parseFloat(cs.getPropertyValue("--top")) || 30;
    const W = screen.clientWidth - margin * 2, H = screen.clientHeight - top - 36;
    reading.style.left = margin + "px";
    reading.style.top = top + "px";
    reading.style.width = W + "px";
    reading.style.height = H + "px";
    flow.style.columnWidth = W + "px";
    flow.style.columnGap = margin * 2 + "px";
    return { W, gap: margin * 2 };
  }

  function applyCrops() {
    for (const frame of flow.querySelectorAll(".frame[data-crop]")) {
      const img = frame.querySelector("img");
      const [x, y, cw, ch] = frame.dataset.crop.split(" ").map(Number);
      const lay = () => {
        if (!img.naturalWidth) return;
        frame.style.aspectRatio = (cw * img.naturalWidth) + " / " + (ch * img.naturalHeight);
        img.style.maxWidth = "none";
        img.style.width = (100 / cw) + "%";
        img.style.marginLeft = (-x / cw * 100) + "%";
        img.style.marginTop = (-y / cw * (img.naturalHeight / img.naturalWidth) * 100) + "%";
        paginate(false);
      };
      if (img.complete) lay(); else img.addEventListener("load", lay, { once: true });
    }
  }

  function paginate(keepPage) {
    const g = geometry();
    const step = g.W + g.gap;
    pages = Math.max(1, Math.round((flow.scrollWidth + g.gap) / step));
    if (!keepPage) page = Math.min(page, pages - 1);
    page = Math.max(0, Math.min(page, pages - 1));
    flow.style.transform = "translateX(" + (-page * step) + "px)";
    const ch = chapters[current];
    $("where").textContent = ch ? ch.title : "";
    $("progress").textContent = pages > 1 ? (page + 1) + " of " + pages : "";
    send({ type: "position", chapter: current, page, pages });
  }

  function show(index, atEnd) {
    if (!chapters.length) return;
    current = Math.max(0, Math.min(index, chapters.length - 1));
    document.body.classList.add("instant");
    flow.innerHTML = chapters[current].html;
    flow.querySelectorAll("a").forEach((a) => a.addEventListener("click", (e) => e.preventDefault()));
    page = 0;
    paginate(true);
    if (atEnd) { page = pages - 1; paginate(true); }
    applyCrops();
    requestAnimationFrame(() => document.body.classList.remove("instant"));
  }

  function turn(delta) {
    if (delta > 0 && page + 1 < pages) { page++; paginate(true); }
    else if (delta < 0 && page > 0) { page--; paginate(true); }
    else if (delta > 0 && current + 1 < chapters.length) show(current + 1, false);
    else if (delta < 0 && current > 0) show(current - 1, true);
  }

  function applySettings() {
    device.dataset.device = settings.device;
    document.body.dataset.theme = settings.theme;
    document.body.classList.toggle("dark", !!settings.dark);
    flow.style.setProperty("--scale", settings.scale);
    const families = { book: "'BookFont', Georgia, serif", serif: "Georgia, 'Times New Roman', serif", sans: "'Segoe UI', Arial, sans-serif" };
    flow.style.setProperty("--book-font", families[settings.font] || families.book);
    fitDevice();
    // Re-flow, keeping roughly the same place in the chapter.
    const frac = pages > 1 ? page / pages : 0;
    paginate(true);
    page = Math.round(frac * pages);
    paginate(true);
  }

  function receive(m) {
    if (m.type === "load") {
      chapters = m.chapters || [];
      bookStyle.textContent = m.css || "";
      const f = m.fonts || {};
      const face = (file, weight, style) => file ? "@font-face { font-family: 'BookFont'; src: url('https://fonts.nrstudio.localhost/" + file + "'); font-weight: " + weight + "; font-style: " + style + "; }\n" : "";
      fontCss = face(f.regular, 400, "normal") + face(f.italic, 400, "italic") + face(f.bold, 700, "normal") + face(f.boldItalic, 700, "italic");
      fontStyle.textContent = fontCss;
      document.documentElement.lang = m.language || "en-GB";
      if (m.settings) settings = Object.assign(settings, m.settings);
      applySettings();
      show(m.chapter || 0, false);
      if (document.fonts) document.fonts.ready.then(() => paginate(true));
    } else if (m.type === "settings") {
      settings = Object.assign(settings, m);
      applySettings();
    } else if (m.type === "goto") {
      show(m.chapter, false);
    } else if (m.type === "turn") {
      turn(m.delta);
    }
  }
  if (host) host.addEventListener("message", (e) => receive(typeof e.data === "string" ? JSON.parse(e.data) : e.data));
  window.studioReceive = receive;

  // Tap the right two thirds of the screen for the next page, the left third for the previous.
  screen.addEventListener("click", (e) => {
    const r = screen.getBoundingClientRect();
    turn(e.clientX - r.left > r.width / 3 ? 1 : -1);
  });
  window.addEventListener("keydown", (e) => {
    if (["ArrowRight", "PageDown", " "].includes(e.key)) { turn(1); e.preventDefault(); }
    else if (["ArrowLeft", "PageUp"].includes(e.key)) { turn(-1); e.preventDefault(); }
  });
  window.addEventListener("wheel", (e) => { if (Math.abs(e.deltaY) > 30) turn(e.deltaY > 0 ? 1 : -1); }, { passive: true });
  window.addEventListener("resize", () => { fitDevice(); paginate(true); });
  fitDevice();
  send({ type: "ready" });
})();

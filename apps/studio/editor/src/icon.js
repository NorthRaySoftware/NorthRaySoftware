// NorthRay Writing Studio: a bible page's icon in the editor (phase 4b, 0.102), drawn as the app draws it: its picture
// with its symbol as a badge, else its symbol (a character or a small picture) or its initials on a disc of its colour
// in ink chosen to read on it. `icon` is what the app sends: {colour, ink, image, symbol, symbolImage, initials}.

const OBJECTS = "https://objects.nrstudio.localhost/";
const HEX = /^#[0-9a-fA-F]{6}$/;
const BLOB = /^[0-9a-f]{64}$/;
const url = (blob) => OBJECTS + blob.slice(0, 2) + "/" + blob.slice(2);

function initialsOf(name) {
  const parts = String(name || "?").trim().split(/\s+/).filter(Boolean);
  return ((parts[0] || "?")[0] + (parts.length > 1 ? parts[parts.length - 1][0] : "")).toUpperCase();
}

// The symbol or initials on the colour, filling `el` (a disc).
function fillDisc(el, icon, name) {
  if (BLOB.test(icon.symbolImage || "")) {
    const img = document.createElement("img");
    img.className = "sym-img";
    img.alt = "";
    img.src = url(icon.symbolImage);
    el.append(img);
  } else {
    const s = document.createElement("span");
    s.className = "mark" + (icon.symbol ? " sym" : "");
    s.textContent = icon.symbol ? String(icon.symbol).slice(0, 8) : icon.initials || initialsOf(name);
    el.append(s);
  }
}

// A span.nr-icon; its size comes from CSS (--s). `name` is for initials when the app sent none.
export function iconFor(icon, name) {
  const ic = icon && typeof icon === "object" ? icon : {};
  const el = document.createElement("span");
  el.className = "nr-icon";
  el.setAttribute("aria-hidden", "true");
  if (HEX.test(ic.colour || "")) el.style.setProperty("--c", ic.colour);
  if (HEX.test(ic.ink || "")) el.style.setProperty("--ink", ic.ink);
  if (BLOB.test(ic.image || "")) {
    const img = document.createElement("img");
    img.className = "face";
    img.alt = "";
    img.src = url(ic.image);
    // A picture that won't load: the disc instead.
    img.addEventListener("error", () => { el.replaceChildren(); el.classList.remove("has-face"); fillDisc(el, ic, name); });
    el.append(img);
    el.classList.add("has-face");
    if (ic.symbol || BLOB.test(ic.symbolImage || "")) {
      const b = document.createElement("span");
      b.className = "badge";
      fillDisc(b, ic, name);
      el.append(b);
    }
  } else fillDisc(el, ic, name);
  return el;
}

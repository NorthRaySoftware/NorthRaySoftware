// NorthRay Writing Studio: the manuscript editor.
//
// Runs inside WebView2. The C++ side owns the text (it's saved in the
// library); this page edits one chapter or scene at a time and reports
// changes. The schema matches studio/document.hpp exactly, so a document
// round-trips without loss.
//
// Messages from the app:  {type:"load", id, doc, title, language, dark, readOnly, focus, placeholder}
//                         {type:"flush"} (answered with {type:"flushed"}) {type:"title", title}  {type:"theme", dark}  {type:"image", blob, alt}  {type:"focus"}
//                         {type:"saved", id, ok} (after a change was stored)  {type:"command", name} (the Format menu)
// Messages to the app:    {type:"ready"} {type:"change", id, doc, words} {type:"stats", id, words, selectionWords}
//                         {type:"link", href} {type:"insertImage"} {type:"pasteImage", mime, name, data(base64) | tooLarge} {type:"saved-state", dirty} {type:"navigate", delta}
//                         {type:"picture", id, pos, attrs} (a picture selected; pos null when none is)
//                         {type:"comment", id, node, quote} (a comment was made on the selection)
//                         {type:"commentFocus", id, node} (the cursor is in a comment; id null when it isn't)
// Comments from the app:  {type:"comments", resolved:[ids]} {type:"selectComment", id} {type:"stripComment", id}
// Pictures from the app:  {type:"image", blob, alt, width} {type:"setPicture", pos, attrs} {type:"removePicture", pos}
// Style rules:            {type:"view", ..., style:{scope, name, label, rules, ignore}} from the app;
//                         {type:"styleRules", rules, ignore} back to save; {type:"styleExport", data} / {type:"styleImport"};
//                         {type:"styleImported", data} from the app (a file it read)
// Writing view:           {type:"view", settings, names:[{id, names:[...]}]} from the app; {type:"view", settings} back when
//                         the writer changes it (font, size, width, focus, typewriter, spellcheck, hints, names).
//                         {type:"entry", id} (Ctrl+click on a name from the world's bible)

import { clampZoom, stepZoom, pageWidth } from "./zoom.js";
import { Schema } from "prosemirror-model";
import { cleanPastedHTML, tidySlice, textToSlice } from "./paste.js";
import { EditorState, Plugin, TextSelection, NodeSelection } from "prosemirror-state";
import { EditorView, Decoration, DecorationSet } from "prosemirror-view";
import { baseKeymap, toggleMark, setBlockType, wrapIn, chainCommands, exitCode, lift } from "prosemirror-commands";
import { keymap } from "prosemirror-keymap";
import { history, undo, redo, closeHistory } from "prosemirror-history";
import { inputRules, InputRule, smartQuotes, ellipsis, emDash, wrappingInputRule, textblockTypeInputRule } from "prosemirror-inputrules";
import { splitListItem, liftListItem, sinkListItem, wrapInList } from "prosemirror-schema-list";
import { dropCursor } from "prosemirror-dropcursor";
import { gapCursor } from "prosemirror-gapcursor";
import { analyse } from "./hints.js";
import * as spell from "./spell.js";
import { initStyleUi, renderList, renderRules, imported as styleImported, otherWords } from "./style_ui.js";
import { initNameCard, setEntries, showNameCard, kindClass } from "./namecard.js";
import { iconFor } from "./icon.js";

// ------------------------------------------------------------------ schema

export const STYLES = [
  ["body", "Body"], ["first", "First paragraph"], ["centered", "Centred"], ["epigraph", "Epigraph"],
  ["attribution", "Attribution"], ["verse", "Verse"], ["letter", "Letter"], ["note", "Note"],
  ["draft", "Draft note"],   // 0.94: a paragraph to yourself, never printed or exported
];
// Comic scripts (a book marked as a comic): numbered for the writer as they type.
export const COMIC_STYLES = [["page", "Page"], ["panel", "Panel"], ["dialogue", "Dialogue"], ["caption", "Caption"], ["sfx", "SFX"]];
const COMIC_IDS = COMIC_STYLES.map((s) => s[0]);
const STYLE_IDS = [...STYLES.map((s) => s[0]), ...COMIC_IDS];
let comicMode = false;
const OBJECTS = "https://objects.nrstudio.localhost/";

export const schema = new Schema({
  nodes: {
    doc: { content: "block+" },
    paragraph: {
      group: "block", content: "inline*",
      attrs: { style: { default: "body" } },
      parseDOM: [{ tag: "p", getAttrs: (d) => ({ style: STYLE_IDS.includes(d.dataset.style) ? d.dataset.style : "body" }) }],
      toDOM: (n) => ["p", { "data-style": n.attrs.style, class: "s-" + n.attrs.style }, 0],
    },
    heading: {
      group: "block", content: "inline*", defining: true,
      attrs: { level: { default: 2 } },
      parseDOM: [1, 2, 3].map((l) => ({ tag: "h" + l, attrs: { level: l } })),
      toDOM: (n) => ["h" + n.attrs.level, 0],
    },
    blockquote: {
      group: "block", content: "block+", defining: true,
      parseDOM: [{ tag: "blockquote" }], toDOM: () => ["blockquote", 0],
    },
    bullet_list: { group: "block", content: "list_item+", parseDOM: [{ tag: "ul" }], toDOM: () => ["ul", 0] },
    ordered_list: {
      group: "block", content: "list_item+", attrs: { start: { default: 1 } },
      parseDOM: [{ tag: "ol", getAttrs: (d) => ({ start: +d.getAttribute("start") || 1 }) }],
      toDOM: (n) => ["ol", n.attrs.start === 1 ? {} : { start: n.attrs.start }, 0],
    },
    list_item: { content: "paragraph block*", defining: true, parseDOM: [{ tag: "li" }], toDOM: () => ["li", 0] },
    scene_break: {
      group: "block", atom: true, selectable: true,
      parseDOM: [{ tag: "hr" }], toDOM: () => ["div", { class: "scene-break", contenteditable: "false" }, ["span", "*  *  *"]],
    },
    image: {
      group: "block", atom: true, draggable: true,
      // Placement (crop as fractions of the picture, width as a fraction of the text width) is
      // never applied to the stored file: it's how the picture sits here, and can always be changed.
      attrs: { blob: {}, alt: { default: "" }, caption: { default: "" }, cropX: { default: 0 }, cropY: { default: 0 },
        cropW: { default: 1 }, cropH: { default: 1 }, width: { default: 1 }, align: { default: "center" } },
      parseDOM: [{ tag: "figure[data-blob]", getAttrs: (d) => ({ blob: d.dataset.blob, alt: d.dataset.alt || "", caption: d.dataset.caption || "" }) }],
      toDOM: (n) => ["figure", { "data-blob": n.attrs.blob, "data-alt": n.attrs.alt, "data-caption": n.attrs.caption, contenteditable: "false" },
        ["img", { src: OBJECTS + n.attrs.blob.slice(0, 2) + "/" + n.attrs.blob.slice(2), alt: n.attrs.alt }],
        ...(n.attrs.caption ? [["figcaption", n.attrs.caption]] : [])],
    },
    text: { group: "inline" },
    hard_break: {
      inline: true, group: "inline", selectable: false,
      parseDOM: [{ tag: "br" }], toDOM: () => ["br"],
    },
  },
  marks: {
    em: { parseDOM: [{ tag: "i" }, { tag: "em" }, { style: "font-style=italic" }, { style: "font-style=normal", clearMark: (m) => m.type.name === "em" }],
      toDOM: () => ["em", 0] },
    strong: { parseDOM: [{ tag: "b" }, { tag: "strong" }, { style: "font-weight=400", clearMark: (m) => m.type.name === "strong" },
      { style: "font-weight=normal", clearMark: (m) => m.type.name === "strong" },
      { style: "font-weight", getAttrs: (v) => /^(bold(er)?|[6-9]\d\d)$/.test(v) && null }], toDOM: () => ["strong", 0] },
    underline: { parseDOM: [{ tag: "u" }, { style: "text-decoration=underline" }, { style: "text-decoration-line=underline" }], toDOM: () => ["u", 0] },
    smallcaps: { parseDOM: [{ tag: "span.sc" }, { style: "font-variant=small-caps" }], toDOM: () => ["span", { class: "sc" }, 0] },
    strike: { parseDOM: [{ tag: "s" }, { tag: "del" }], toDOM: () => ["s", 0] },
    sup: { excludes: "sub", parseDOM: [{ tag: "sup" }], toDOM: () => ["sup", 0] },
    sub: { excludes: "sup", parseDOM: [{ tag: "sub" }], toDOM: () => ["sub", 0] },
    code: { parseDOM: [{ tag: "code" }], toDOM: () => ["code", 0] },
    // A note to yourself in the middle of the text, "[check this date]" (0.94): shown here, never printed or exported.
    draft: { parseDOM: [{ tag: "span.draft" }], toDOM: () => ["span", { class: "draft", title: "Draft note: not printed or exported" }, 0] },
    // A world's own typeface on these words: runes, symbols, a character's handwriting (0.41). By the family's id.
    font: {
      attrs: { family: {} },
      parseDOM: [{ tag: "span[data-font]", getAttrs: (d) => (/^[a-z0-9-]{1,64}$/.test(d.dataset.font) ? { family: d.dataset.font } : false) }],
      toDOM: (m) => ["span", { class: "ff ff-" + m.attrs.family, "data-font": m.attrs.family }, 0],
    },
    // Where a comment sits in the text. Its words live in the library, by id.
    // Several can overlap, so a comment doesn't exclude another one.
    comment: {
      attrs: { id: {} }, inclusive: false, excludes: "",
      parseDOM: [{ tag: "span[data-comment]", getAttrs: (d) => (/^[0-9a-f]{8,32}$/.test(d.dataset.comment) ? { id: d.dataset.comment } : false) }],
      toDOM: (m) => ["span", { class: "cm", "data-comment": m.attrs.id }, 0],
    },
    link: {
      attrs: { href: {} }, inclusive: false,
      parseDOM: [{ tag: "a[href]", getAttrs: (d) => (/^(https?:|mailto:)/.test(d.getAttribute("href")) ? { href: d.getAttribute("href") } : false) }],
      toDOM: (m) => ["a", { href: m.attrs.href, title: m.attrs.href }, 0],
    },
  },
});

// A picture as it will sit on the page: cropped, sized and aligned (a node view, so
// the crop is shown without touching the file).
const objectUrl = (blob) => OBJECTS + blob.slice(0, 2) + "/" + blob.slice(2);
class ImageView {
  constructor(node) {
    this.dom = document.createElement("figure");
    this.dom.contentEditable = "false";
    this.frame = document.createElement("div");
    this.frame.className = "frame";
    this.img = document.createElement("img");
    this.img.draggable = false;
    this.img.addEventListener("load", () => this.layout());
    this.frame.appendChild(this.img);
    this.cap = document.createElement("figcaption");
    this.dom.append(this.frame, this.cap);
    this.update(node);
  }
  update(node) {
    if (node.type !== schema.nodes.image) return false;
    const blobChanged = !this.node || this.node.attrs.blob !== node.attrs.blob;
    this.node = node;
    if (blobChanged) this.img.src = objectUrl(node.attrs.blob);
    this.img.alt = node.attrs.alt || "";
    this.cap.textContent = node.attrs.caption || "";
    this.cap.hidden = !node.attrs.caption;
    this.dom.dataset.align = node.attrs.align || "center";
    this.layout();
    return true;
  }
  layout() {
    const a = this.node.attrs;
    this.frame.style.width = Math.round(Math.min(1, Math.max(0.1, a.width)) * 100) + "%";
    const nw = this.img.naturalWidth, nh = this.img.naturalHeight;
    if (!nw || !nh) return;
    const cw = Math.max(0.05, a.cropW), ch = Math.max(0.05, a.cropH);
    this.frame.style.aspectRatio = (cw * nw) + " / " + (ch * nh);
    this.img.style.width = (100 / cw) + "%";
    this.img.style.marginLeft = (-a.cropX / cw * 100) + "%";
    this.img.style.marginTop = (-a.cropY / cw * (nh / nw) * 100) + "%";
  }
  stopEvent() { return false; }
  ignoreMutation() { return true; }
}

// ------------------------------------------------------------------- words
// The same rule as studio::doc::countWords: letters and digits, joined by ' and -.
const WORD = /[\p{L}\p{N}]+(?:['’\-‐‑][\p{L}\p{N}]+)*/gu;
export function countWords(text) { const m = text.match(WORD); return m ? m.length : 0; }
function docWords(doc) { return countWords(doc.textBetween(0, doc.content.size, "\n\n", (n) => (n.type.name === "hard_break" ? " " : ""))); }

// ---------------------------------------------------------------- bridge

const host = window.chrome && window.chrome.webview;
function send(msg) { if (host) host.postMessage(msg); else window.dispatchEvent(new CustomEvent("studio-out", { detail: msg })); }

// ----------------------------------------------------------------- commands

const S = schema.nodes, M = schema.marks;

function setStyle(style) {
  return (state, dispatch) => {
    const { $from, $to } = state.selection;
    let ok = false;
    const tr = state.tr;
    state.doc.nodesBetween($from.pos, $to.pos, (node, pos) => {
      if (node.type === S.paragraph) { tr.setNodeMarkup(pos, null, { style }); ok = true; return false; }
      if (node.type === S.heading) { tr.setNodeMarkup(pos, S.paragraph, { style }); ok = true; return false; }
      return true;
    });
    if (ok && dispatch) dispatch(tr.scrollIntoView());
    return ok;
  };
}

// Ctrl+Backspace / Ctrl+Delete: the word before or after the cursor (with the spaces next to it),
// within the paragraph; a selection is simply deleted.
function deleteWord(dir) {
  return (state, dispatch) => {
    const { $from, empty, from, to } = state.selection;
    if (!empty) { if (dispatch) dispatch(state.tr.delete(from, to).scrollIntoView()); return true; }
    const p = $from.parent;
    if (!p.isTextblock) return false;
    const text = p.textBetween(0, p.content.size, "\n", "\ufffc");
    let i = $from.parentOffset;
    if (dir < 0 && i === 0) return false;              // at the start: Backspace joins paragraphs as usual
    if (dir > 0 && i === text.length) return false;
    const isWord = (c) => /[\p{L}\p{N}'’_-]/u.test(c);
    if (dir < 0) {
      let j = i;
      while (j > 0 && /\s/.test(text[j - 1])) j--;
      if (j > 0 && isWord(text[j - 1])) while (j > 0 && isWord(text[j - 1])) j--;
      else if (j === i) j--;                            // punctuation: one character
      if (dispatch) dispatch(state.tr.delete($from.start() + j, $from.start() + i).scrollIntoView());
    } else {
      let j = i;
      if (isWord(text[j])) while (j < text.length && isWord(text[j])) j++;
      else if (!/\s/.test(text[j])) j++;
      while (j < text.length && /\s/.test(text[j])) j++;
      if (dispatch) dispatch(state.tr.delete($from.start() + i, $from.start() + j).scrollIntoView());
    }
    return true;
  };
}

// ----------------------------------------------------------------- comic scripts

function comicStyle(style) {
  return (state, dispatch) => (comicMode ? setStyle(style)(state, dispatch) : false);
}

// Enter at the end of a comic paragraph: the next line takes the likely next style
// (after a page, a panel; after a panel, a caption or dialogue line; dialogue follows dialogue).
const NEXT_STYLE = { page: "panel", panel: "dialogue", dialogue: "dialogue", caption: "dialogue", sfx: "dialogue" };
function comicEnter(state, dispatch) {
  if (!comicMode) return false;
  const { $from, empty } = state.selection;
  const p = $from.parent;
  if (!empty || p.type !== S.paragraph || !NEXT_STYLE[p.attrs.style] || $from.parentOffset !== p.content.size) return false;
  // Enter on an empty dialogue line ends the run of dialogue: back to a panel.
  if (p.content.size === 0 && p.attrs.style !== "page" && p.attrs.style !== "panel") {
    if (dispatch) dispatch(state.tr.setNodeMarkup($from.before(), null, { style: "panel" }));
    return true;
  }
  if (!dispatch) return true;
  const tr = state.tr.split($from.pos, 1, [{ type: S.paragraph, attrs: { style: NEXT_STYLE[p.attrs.style] } }]);
  dispatch(tr.scrollIntoView());
  return true;
}

const SPELLED = ["Zero", "One", "Two", "Three", "Four", "Five", "Six", "Seven", "Eight", "Nine", "Ten", "Eleven", "Twelve", "Thirteen",
  "Fourteen", "Fifteen", "Sixteen", "Seventeen", "Eighteen", "Nineteen"];
const TENS = ["", "", "Twenty", "Thirty", "Forty", "Fifty", "Sixty", "Seventy", "Eighty", "Ninety"];
function spelled(n) { return n < 20 ? SPELLED[n] : n < 100 ? TENS[Math.floor(n / 10)] + (n % 10 ? "-" + SPELLED[n % 10] : "") : String(n); }

// The numbers a comic script shows: PAGE ONE (right · four panels), Panel 1, balloon numbers, the speaker in bold.
// Pages count on from the book's earlier chapters (sent by the app as `comicPageStart`).
let comicPageStart = 0;
const comicLook = new Plugin({
  props: {
    decorations(state) {
      if (!comicMode) return null;
      const decos = [];
      const doc = state.doc;
      // Panels per page first.
      const counts = [];
      doc.forEach((n) => {
        if (n.type !== S.paragraph) return;
        if (n.attrs.style === "page") counts.push(0);
        else if (n.attrs.style === "panel") { if (!counts.length) counts.push(0); counts[counts.length - 1]++; }
      });
      let page = comicPageStart, pageIdx = -1, panel = 0, balloon = 0;
      doc.forEach((n, pos) => {
        if (n.type !== S.paragraph) return;
        const st = n.attrs.style;
        let label = null;
        if (st === "page") {
          page++; pageIdx++; panel = 0; balloon = 0;
          const c = counts[pageIdx] || 0;
          label = `PAGE ${spelled(page).toUpperCase()} (${page % 2 ? "right" : "left"}${c ? ` \u00B7 ${spelled(c).toLowerCase()} panel${c === 1 ? "" : "s"}` : ""})`;
        } else if (st === "panel") label = `Panel ${++panel}`;
        else if (st === "dialogue") label = `${++balloon}.`;
        else if (st === "caption") label = `${++balloon}. CAPTION:`;
        else if (st === "sfx") label = "SFX:";
        if (label) decos.push(Decoration.node(pos, pos + n.nodeSize, { "data-label": label }));
        if (st === "dialogue") {
          const t = n.textContent, colon = t.indexOf(":");
          if (colon > 0 && colon < 48) decos.push(Decoration.inline(pos + 1, pos + 1 + colon + 1, { class: "cs-name" }));
        }
      });
      return DecorationSet.create(doc, decos);
    },
  },
});

function insertSceneBreak(state, dispatch) {
  const { $from } = state.selection;
  if (dispatch) {
    let tr = state.tr;
    // On an empty paragraph, replace it; otherwise put the break after this block.
    const parent = $from.parent;
    if (parent.type === S.paragraph && parent.content.size === 0 && $from.depth === 1) {
      tr = tr.replaceWith($from.before(), $from.after(), [S.scene_break.create(), S.paragraph.create()]);
      tr = tr.setSelection(TextSelection.create(tr.doc, $from.before() + 2));
    } else {
      const after = $from.after(1);
      tr = tr.insert(after, [S.scene_break.create(), S.paragraph.create()]);
      tr = tr.setSelection(TextSelection.create(tr.doc, after + 2));
    }
    dispatch(tr.scrollIntoView());
  }
  return true;
}

// "+ Scene": a scene break at the end of the chapter, with the cursor on the
// new scene's first line (an empty last line is reused, not left behind).
function newSceneAtEnd(state, dispatch) {
  if (dispatch) {
    const doc = state.doc;
    const last = doc.lastChild;
    let tr = state.tr;
    const before = doc.childCount > 1 ? doc.child(doc.childCount - 2) : null;
    if (last.type === S.paragraph && last.content.size === 0 && (doc.childCount === 1 || before.type === S.scene_break)) {
      // An empty chapter, or a new scene already waiting: nothing to divide, just go there.
      tr = tr.setSelection(TextSelection.create(tr.doc, tr.doc.content.size - 1));
    } else if (last.type === S.paragraph && last.content.size === 0) {
      const start = doc.content.size - last.nodeSize;
      tr = tr.replaceWith(start, doc.content.size, [S.scene_break.create(), S.paragraph.create()]);
      tr = tr.setSelection(TextSelection.create(tr.doc, tr.doc.content.size - 1));
    } else {
      tr = tr.insert(doc.content.size, [S.scene_break.create(), S.paragraph.create()]);
      tr = tr.setSelection(TextSelection.create(tr.doc, tr.doc.content.size - 1));
    }
    dispatch(tr.scrollIntoView());
  }
  return true;
}

const hardBreak = chainCommands(exitCode, (state, dispatch) => {
  if (dispatch) dispatch(state.tr.replaceSelectionWith(S.hard_break.create()).scrollIntoView());
  return true;
});

function toggleLink(state, dispatch) {
  if (state.selection.empty) return false;
  const has = state.doc.rangeHasMark(state.selection.from, state.selection.to, M.link);
  if (has) { if (dispatch) dispatch(state.tr.removeMark(state.selection.from, state.selection.to, M.link)); return true; }
  const href = window.prompt("Link to (https://...)", "https://");
  if (!href || !/^(https?:\/\/|mailto:)/.test(href)) return false;
  if (dispatch) dispatch(state.tr.addMark(state.selection.from, state.selection.to, M.link.create({ href })));
  return true;
}

// ----------------------------------------------------------------- comments

let resolvedComments = new Set();
let activeComment = null;
const newCommentId = () => Array.from(crypto.getRandomValues(new Uint8Array(8)), (b) => b.toString(16).padStart(2, "0")).join("");

// Ctrl+Alt+M: a comment on the selected words. The app asks for what it says.
function addComment(state, dispatch) {
  const { from, to, empty } = state.selection;
  if (empty || readOnly || state.selection instanceof NodeSelection) return false;
  if (dispatch) {
    const id = newCommentId();
    const quote = state.doc.textBetween(from, to, " ", " ").slice(0, 2000);
    dispatch(state.tr.addMark(from, to, M.comment.create({ id })));
    flush();   // stored with its place in the text straight away
    activeComment = id;
    send({ type: "comment", id, node: currentId, quote });
  }
  return true;
}

// The comments under the cursor, innermost (shortest) first.
function commentsAt(state) {
  const { $from } = state.selection;
  const marks = state.selection.empty ? $from.marks() : $from.marksAcross(state.selection.$to) || [];
  return marks.filter((m) => m.type === M.comment).map((m) => m.attrs.id);
}

function commentRange(doc, id) {
  let from = -1, to = -1;
  doc.descendants((n, pos) => {
    if (n.isText && n.marks.some((m) => m.type === M.comment && m.attrs.id === id)) {
      if (from < 0) from = pos;
      to = pos + n.nodeSize;
    }
  });
  return from < 0 ? null : { from, to };
}

// Review's "show it": the words are selected, brought to the middle of the window and lit up for a moment
// (even when the comment is resolved).
let flashComment = null, flashTimer = null;
function selectComment(id) {
  if (!view) return;
  activeComment = id;
  const r = commentRange(view.state.doc, id);
  if (r) {
    flashComment = id;
    clearTimeout(flashTimer);
    flashTimer = setTimeout(() => { flashComment = null; if (view) view.dispatch(view.state.tr.setMeta("comments", true)); }, 2200);
    const tr = view.state.tr.setSelection(TextSelection.create(view.state.doc, r.from, r.to));
    tr.setMeta("comments", true);
    view.dispatch(tr);
    requestAnimationFrame(() => {
      const sc = document.getElementById("scroller");
      let at;
      try { at = view.coordsAtPos(r.from); } catch (e) { return; }
      if (sc && at) sc.scrollTop += at.top - sc.getBoundingClientRect().top - sc.clientHeight * 0.4;
    });
  } else view.dispatch(view.state.tr.setMeta("comments", true));
}

function stripComment(id) {
  if (!view) return;
  const r = commentRange(view.state.doc, id);
  const tr = view.state.tr;
  if (r) tr.removeMark(r.from, r.to, M.comment.create({ id }));
  // removeMark matches by attributes: make sure every piece goes.
  view.state.doc.descendants((n, pos) => {
    for (const m of n.marks) if (m.type === M.comment && m.attrs.id === id) tr.removeMark(pos, pos + n.nodeSize, m);
  });
  tr.setMeta("comments", true);
  view.dispatch(tr);
  if (activeComment === id) activeComment = null;
  flush();
}

// Resolved comments keep their place in the text (so they can be reopened) but aren't highlighted.
const commentLook = new Plugin({
  props: {
    decorations(state) {
      const decos = [];
      state.doc.descendants((n, pos) => {
        if (!n.isText) return;
        for (const m of n.marks) {
          if (m.type !== M.comment) continue;
          if (m.attrs.id === flashComment) { decos.push(Decoration.inline(pos, pos + n.nodeSize, { class: "cm-open cm-active cm-flash" })); continue; }
          if (resolvedComments.has(m.attrs.id)) continue;
          const cls = m.attrs.id === activeComment ? "cm-open cm-active" : "cm-open";
          decos.push(Decoration.inline(pos, pos + n.nodeSize, { class: cls }));
        }
      });
      return DecorationSet.create(state.doc, decos);
    },
  },
});

let reportedComment = "";
function reportComment(state) {
  const open = commentsAt(state).filter((id) => !resolvedComments.has(id));
  const id = open.length ? open[open.length - 1] : null;
  if (id) activeComment = id;
  const key = id || "";
  if (key === reportedComment) return;
  reportedComment = key;
  send({ type: "commentFocus", id, node: currentId });
}

// Ctrl+Up / Ctrl+Down: the previous or next chapter (the app picks it and loads it).
const navigate = (delta) => () => { flush(); send({ type: "navigate", delta }); return true; };

const keys = {
  "Mod-ArrowUp": navigate(-1), "Mod-ArrowDown": navigate(1),
  "Mod-z": undo, "Mod-y": redo, "Shift-Mod-z": redo,
  "Mod-Backspace": deleteWord(-1), "Mod-Delete": deleteWord(1),
  "Mod-b": toggleMark(M.strong), "Mod-i": toggleMark(M.em), "Mod-u": toggleMark(M.underline),
  "Mod-=": () => zoomPage(1), "Mod-+": () => zoomPage(1), "Mod-Shift-=": () => zoomPage(1), "Mod--": () => zoomPage(-1), "Mod-0": () => zoomPage(0),
  "Mod-Shift-k": toggleMark(M.smallcaps), "Mod-Shift-d": toggleMark(M.draft), "Mod-Alt-k": toggleLink, "Mod-Alt-m": addComment,
  // The app's own: go anywhere (Ctrl+K) and the shortcuts sheet (Ctrl+/) (0.35).
  "Mod-k": () => { flush(); send({ type: "appCommand", name: "jump" }); return true; },
  "Mod-/": () => { send({ type: "appCommand", name: "shortcuts" }); return true; },
  "Mod-Enter": insertSceneBreak, "Shift-Enter": hardBreak,
  "Mod-Alt-0": setStyle("body"), "Mod-Alt-1": setBlockType(S.heading, { level: 1 }),
  "Mod-Alt-2": setBlockType(S.heading, { level: 2 }), "Mod-Alt-3": setBlockType(S.heading, { level: 3 }),
  "Enter": chainCommands(comicEnter, splitListItem(S.list_item)), "Tab": sinkListItem(S.list_item), "Shift-Tab": liftListItem(S.list_item),
  "Mod-[": lift,
  "Alt-1": comicStyle("page"), "Alt-2": comicStyle("panel"), "Alt-3": comicStyle("dialogue"), "Alt-4": comicStyle("caption"), "Alt-5": comicStyle("sfx"),
  "F3": () => { send({ type: "togglePanel", which: "side" }); return true; },
  "F4": () => { send({ type: "togglePanel", which: "details" }); return true; },
  "Mod-Shift-f": toggleFocus, "Mod-Shift-t": toggleTypewriter, "Mod-Shift-o": () => { otherWords(); return true; },
};

const rules = inputRules({
  rules: [
    ...smartQuotes, ellipsis, emDash,
    wrappingInputRule(/^\s*>\s$/, S.blockquote),
    wrappingInputRule(/^\s*([-+*])\s$/, S.bullet_list),
    wrappingInputRule(/^(\d+)\.\s$/, S.ordered_list, (m) => ({ start: +m[1] }), (m, n) => n.childCount + n.attrs.start === +m[1]),
    textblockTypeInputRule(/^(#{1,3})\s$/, S.heading, (m) => ({ level: m[1].length })),
    // Comic scripts: "PAGE ", "PANEL ", "CAPTION: ", "SFX: " start those lines; "TOM: " starts a dialogue line.
    new InputRule(/^(PAGE|PANEL|CAPTION:|SFX:)\s$/i, (state, match, start, end) => {
      if (!comicMode) return null;
      const $s = state.doc.resolve(start);
      if ($s.parent.type !== S.paragraph || $s.depth !== 1) return null;
      const style = { page: "page", panel: "panel", "caption:": "caption", "sfx:": "sfx" }[match[1].toLowerCase()];
      return state.tr.delete(start, end).setNodeMarkup($s.before(), null, { style });
    }),
    new InputRule(/^([A-Z][A-Z0-9'’ .()\-]{0,30}):\s$/, (state, match, start) => {
      if (!comicMode) return null;
      const $s = state.doc.resolve(start);
      if ($s.parent.type !== S.paragraph || $s.depth !== 1 || $s.parent.attrs.style === "dialogue" || /^(PAGE|PANEL|CAPTION|SFX)$/.test(match[1])) return null;
      return state.tr.setNodeMarkup($s.before(), null, { style: "dialogue" }).insertText(" ");
    }),
    // "***" or "* * *" on its own line: a scene break.
    new InputRule(/^(\*\s?\*\s?\*|#)$/, (state, match, start, end) => {
      const $s = state.doc.resolve(start);
      if ($s.parent.type !== S.paragraph || $s.depth !== 1) return null;
      const tr = state.tr.replaceWith($s.before(), $s.after(), [S.scene_break.create(), S.paragraph.create()]);
      return tr.setSelection(TextSelection.create(tr.doc, $s.before() + 2));
    }),
  ],
});

// -------------------------------------------------------------- writing view

const VIEW_DEFAULTS = { v: 3, zoom: 1, font: "serif", size: 1, width: "normal", focus: false, typewriter: false, spellcheck: true, hints: "quiet", hintCats: {}, names: true, paper: "usual", spacing: "normal", heads: true, povName: true, dates: true };
const hintMode = () => (viewSettings.hints === true ? "full" : viewSettings.hints === false ? "off" : ["off", "quiet", "full"].includes(viewSettings.hints) ? viewSettings.hints : "quiet");
let viewSettings = { ...VIEW_DEFAULTS };
const FONTS = {
  serif: '"Cambria", "Georgia", "Times New Roman", serif',
  book: '"Palatino Linotype", "Book Antiqua", "Palatino", "Georgia", serif',
  sans: '"Segoe UI Variable Text", "Segoe UI", system-ui, sans-serif',
  typewriter: '"Courier New", "Consolas", monospace',
};
const SIZES = [1.0, 1.12, 1.26, 1.42];
const SPACINGS = { tight: "1.45", normal: "1.65", loose: "1.9", double: "2.2" };   // line spacing (0.49)

function applyView() {
  const v = viewSettings, root = document.documentElement;
  root.style.setProperty("--text-font", FONTS[v.font] || FONTS.serif);
  root.style.setProperty("--text-size", ((SIZES[v.size] ?? SIZES[1]) * clampZoom(v.zoom)).toFixed(4) + "rem");
  root.style.setProperty("--page-width", pageWidth(v.width, v.zoom));
  root.style.setProperty("--line", SPACINGS[v.spacing] || SPACINGS.normal);
  root.classList.toggle("focus-mode", !!v.focus);
  root.dataset.paper = v.paper === "night" ? "night" : "usual";   // warm night paper for writing in the dark (0.35)
  root.classList.toggle("typewriter", !!v.typewriter);
  root.classList.toggle("noheads", v.heads === false);   // the chapter's header picture and date (0.92)
  root.classList.toggle("nopovname", v.povName === false);   // the viewpoint's icon alone, without the name (0.102)
  root.classList.toggle("nodates", v.dates === false);       // the chapter's date line, apart from the rest (0.105)
  if (view) {
    // The Studio's own checker once its dictionary is loaded (it knows the bible's names); the browser's until then.
    view.dom.setAttribute("spellcheck", v.spellcheck && !spell.ready() ? "true" : "false");
    view.dispatch(view.state.tr.setMeta("view", true));
    keepCaretCentred();
  }
  syncViewMenu();
  requestAnimationFrame(() => placeCaret());
}

function changeView(patch) {
  viewSettings = { ...viewSettings, ...patch };
  applyView();
  send({ type: "view", settings: viewSettings });
}

// Focus mode: everything but the paragraph you're in fades back.
const focusLook = new Plugin({
  props: {
    decorations(state) {
      if (!viewSettings.focus) return null;
      const here = state.selection.$from.index(0);
      const decos = [];
      let i = 0;
      state.doc.forEach((node, pos) => {
        if (i++ !== here) decos.push(Decoration.node(pos, pos + node.nodeSize, { class: "dim" }));
      });
      return DecorationSet.create(state.doc, decos);
    },
  },
});

// The paragraph being written in (0.49): marked "here", so style hints step out of the way while you write in it.
const hereLook = new Plugin({
  props: {
    decorations(state) {
      if (readOnly) return null;
      const $from = state.selection.$from;
      if ($from.depth < 1) return null;
      const start = $from.before(1), node = state.doc.nodeAt(start);
      if (!node) return null;
      return DecorationSet.create(state.doc, [Decoration.node(start, start + node.nodeSize, { class: "here" })]);
    },
  },
});

// Typewriter scrolling: the line you're typing stays a little above the middle of the window.
function keepCaretCentred() {
  if (!view || !viewSettings.typewriter) return;
  const scroller = document.getElementById("scroller");
  let c;
  try { c = view.coordsAtPos(view.state.selection.head); } catch (e) { return; }
  const box = scroller.getBoundingClientRect();
  const target = box.top + box.height * 0.42;
  const delta = (c.top + c.bottom) / 2 - target;
  if (Math.abs(delta) > 2) {
    scroller.style.scrollBehavior = "auto";
    scroller.scrollTop += delta;
    scroller.style.scrollBehavior = "";
  }
}

// Style hints (hints.js): worked out for the whole chapter a moment after typing stops,
// and moved along with the text in between, so typing never waits for them.
let style = { scope: "", name: "this book", label: "this book", rules: [], ignore: {}, words: [], languages: [] };
let ignoreOnceSet = new Set();
let lastHints = { hints: [], counts: {}, slow: [] };
let pausedRules = new Set();
let hintCount = 0;
let rehintTimer = null;

// Each text block's words for hints and spelling. Words in a world typeface (runes, handwriting) are left out as
// spaces of the same length, so positions still line up: they're not English to check.
function textBlocks(doc) {
  const out = [];
  doc.descendants((n, pos) => {
    if (n.isTextblock) {
      let text = "";
      n.forEach((c) => {
        if (c.isText) text += c.marks.some((m) => m.type === M.font) ? " ".repeat(c.text.length) : c.text;
        else text += " ".repeat(c.nodeSize);
      });
      out.push({ text, pos: pos + 1 });
      return false;
    }
    return true;
  });
  return out;
}

function hintDecorations(doc) {
  const res = analyse(textBlocks(doc), {
    mode: hintMode(), categories: viewSettings.hintCats || {}, rules: style.rules.filter((r) => !pausedRules.has(r.id)),
    ignore: style.ignore, ignoreOnce: ignoreOnceSet, names: nameWords, lang: document.documentElement.lang,
    isWord: spell.ready() ? spell.isWord : null,
  });
  for (const id of res.slow) if (!pausedRules.has(id)) {
    pausedRules.add(id);
    const r = style.rules.find((x) => x.id === id);
    notice(`Your rule "${r ? r.name : id}" took too long on this chapter, so it's paused until the Studio is opened again.`);
  }
  hintCount = res.hints.length;
  // Spelling, in the same pass (not counted as style hints).
  if (viewSettings.spellcheck && spell.ready()) {
    const spellIgnored = new Set(style.ignore["spell"] || []);
    for (const b of textBlocks(doc))
      for (const m of spell.misspelt(b.text)) {
        const context = b.text.slice(Math.max(0, m.from - 24), m.to + 24);
        const low = m.word.toLocaleLowerCase();
        if (spellIgnored.has(low) || ignoreOnceSet.has("spell|" + low + "|" + context)) continue;
        // Near a word of one of the bible's languages: it's that word misspelt.
        const near = spell.conlangNear(m.word);
        const dict = `the ${document.documentElement.lang === "en-US" ? "American" : "British"} English dictionary`;
        const msg = near
          ? `"${m.word}" isn't in ${dict} or in ${near.lang || "your language"}. Did you mean the ${near.lang || "language"} word "${near.word}"${near.meaning ? ` (${near.meaning})` : ""}?`
          : `"${m.word}" isn't in ${dict}${style.languages && style.languages.length ? " or your world's languages" : ""}.`;
        res.hints.push({ from: b.pos + m.from, to: b.pos + m.to, key: "spell", cat: "spell", name: "Spelling", text: m.word, context, msg, why: "" });
      }
  }
  lastHints = res;
  setTimeout(() => { renderList(); }, 0);
  return DecorationSet.create(doc, res.hints.map((h, i) =>
    Decoration.inline(h.from, h.to, { class: "hint hint-" + (h.cat || "rule"), "data-hint": String(i) })));
}

function rehintSoon() {
  clearTimeout(rehintTimer);
  // Only once typing has paused for a moment (0.35): new hints never pop up mid-sentence.
  rehintTimer = setTimeout(() => { if (view) view.dispatch(view.state.tr.setMeta("rehint", true)); }, 1400);
}

// A short message in the toolbar (the app has no status bar).
let noticeTimer = null;
function notice(text) {
  const n = document.getElementById("notice");
  if (!n) return;
  n.textContent = text;
  n.hidden = false;
  clearTimeout(noticeTimer);
  noticeTimer = setTimeout(() => { n.hidden = true; }, 7000);
}

// Names from the world's bible: a dotted underline; Ctrl+click opens the entry in Worldbuilding.
let nameList = [];        // [{id, names: [...]}]
let namePattern = null;   // one regular expression for all of them
let nameIds = new Map();  // lower-cased name -> entry id
let nameWords = new Set(); // every word of every name, lower-cased (echo hints leave them alone)
let nameKinds = new Map(); // entry id -> its kind's class (k-place, ...), for the link's colour
// The parts of a character's full name that also stand for them: the first name and the
// last name on their own, and runs that include one of them. "Snarf Jill Grave" gives
// Snarf, Grave, Snarf Jill, Jill Grave and Snarf Grave, but not Jill alone (a middle
// name is rarely used by itself). Titles and joining words (Captain, of) don't count.
const PARTICLES = new Set(["of", "the", "de", "da", "di", "du", "del", "della", "der", "den", "van", "von", "le", "la", "al", "el",
  "bin", "bint", "ibn", "ap", "mac", "y", "and"]);
const TITLES = new Set(["mr", "mrs", "ms", "miss", "dr", "sir", "dame", "lord", "lady", "king", "queen", "prince", "princess",
  "captain", "capt", "general", "father", "mother", "brother", "sister", "master", "mistress", "saint", "st", "professor", "prof",
  "her", "his", "your", "my", "their", "our", "highness", "majesty", "lordship", "ladyship", "grace", "excellency", "honour", "honor", "eminence", "holiness"]);
function nameParts(full) {
  const words = String(full).trim().split(/\s+/).filter(Boolean);
  if (words.length < 2) return [];
  const isName = (w) => /^[\p{Lu}]/u.test(w) && !PARTICLES.has(w.toLocaleLowerCase()) && !TITLES.has(w.toLocaleLowerCase().replace(/\.$/, ""));
  const out = new Set();
  const firstAt = words.findIndex(isName);
  let lastAt = -1;
  for (let i = words.length - 1; i >= 0; i--) if (isName(words[i])) { lastAt = i; break; }
  for (let a = 0; a < words.length; a++)
    for (let z = a; z < words.length; z++) {
      if (a === 0 && z === words.length - 1) continue;          // the full name itself
      if (!isName(words[a]) || !isName(words[z])) continue;      // starts and ends on a real name
      if (a !== firstAt && z !== lastAt) continue;               // must include the first or the last name
      out.add(words.slice(a, z + 1).join(" "));
    }
  // Skipping middle names too: "Albus Dumbledore", "Albus Brian Dumbledore" (in order, with the first or last).
  const named = words.filter(isName);
  const k = Math.min(named.length, 8);
  if (k >= 2) {
    const pick = named.length > 8 ? [...named.slice(0, 4), ...named.slice(-4)] : named;
    for (let mask = 1; mask < (1 << k); mask++) {
      if (!(mask & 1) && !(mask & (1 << (k - 1)))) continue;
      const sel = pick.filter((_, i) => mask & (1 << i));
      if (sel.join(" ") !== words.join(" ")) out.add(sel.join(" "));
    }
  }
  return [...out].filter((p) => p.length >= 2);
}

function setNames(list) {
  nameList = Array.isArray(list) ? list : [];
  nameIds = new Map();   // lower-cased name -> [entry ids]: two characters can share a name
  nameWords = new Set();
  nameKinds = new Map(nameList.map((e) => [e.id, kindClass(e.kind)]));
  const all = new Set();
  const add = (t, id) => {
    t = String(t).trim();
    if (t.length < 2) return;
    const k = t.toLocaleLowerCase();
    if (!nameIds.has(k)) nameIds.set(k, []);
    if (!nameIds.get(k).includes(id)) nameIds.get(k).push(id);
    for (const w of k.split(/[^\p{L}\p{N}'’]+/u)) if (w) { nameWords.add(w); nameWords.add(w.replace(/[’‘]/g, "'")); nameWords.add(w.replace(/'/g, "’")); }
    all.add(t);
    all.add(t.toLocaleUpperCase());   // shouted: "JACKSON!"
  };
  // Either apostrophe matches: the bible's Zah'mes is the text's Zah’mes.
  const addBoth = (t, id) => { add(t, id); if (/['’]/.test(t)) { add(t.replace(/’/g, "'"), id); add(t.replace(/'/g, "’"), id); } };
  for (const e of nameList) {
    for (const n of e.names || []) addBoth(n, e.id);
    if (e.kind === "Character") for (const n of e.names || []) for (const p of nameParts(n)) addBoth(p, e.id);
  }
  const sorted = [...all].sort((a, b) => b.length - a.length);   // "Maren Holt" before "Maren"
  const esc = sorted.map((t) => t.replace(/[.*+?^${}()|[\]\\]/g, "\\$&"));
  namePattern = esc.length ? new RegExp("(?<![\\p{L}\\p{N}])(" + esc.join("|") + ")(?![\\p{L}\\p{N}])", "gu") : null;
}

function nameDecorations(doc) {
  if (!namePattern) return DecorationSet.empty;
  const decos = [];
  doc.descendants((node, pos) => {
    if (!node.isTextblock) return true;
    const text = node.textBetween(0, node.content.size, " ", " ");
    for (const m of text.matchAll(namePattern)) {
      const ids = nameIds.get(m[1].toLocaleLowerCase());
      if (!ids || !ids.length) continue;
      // Hovering shows its card (every one, when characters share the name); Ctrl+click opens it.
      decos.push(Decoration.inline(pos + 1 + m.index, pos + 1 + m.index + m[0].length,
        { class: (ids.length > 1 ? "wl wl-shared " : "wl ") + (nameKinds.get(ids[0]) || ""), "data-entry": ids.join(" ") }));
    }
    return false;
  });
  return DecorationSet.create(doc, decos);
}

// Names are found again with each change (one pattern, quick). Hints follow the text
// as it moves and are worked out again a moment after typing stops.
const lookups = new Plugin({
  state: {
    init: (_, state) => ({ hints: hintDecorations(state.doc), names: viewSettings.names ? nameDecorations(state.doc) : DecorationSet.empty }),
    apply(tr, old, _prev, state) {
      const now = tr.getMeta("view") || tr.getMeta("rehint");
      if (!tr.docChanged && !now) return old;
      if (tr.docChanged && !now) rehintSoon();
      return {
        hints: now ? hintDecorations(state.doc) : old.hints.map(tr.mapping, tr.doc),
        names: viewSettings.names ? nameDecorations(state.doc) : DecorationSet.empty,
      };
    },
  },
  props: {
    decorations(state) {
      const s = lookups.getState(state);
      if (s.hints === DecorationSet.empty) return s.names;
      if (s.names === DecorationSet.empty) return s.hints;
      return DecorationSet.create(state.doc, [...s.hints.find(), ...s.names.find()]);
    },
  },
});

function saveStyle() { send({ type: "styleRules", rules: style.rules, ignore: style.ignore, words: style.words }); }
function rehintNow() { if (view) view.dispatch(view.state.tr.setMeta("rehint", true)); }

// Moves to the next hint of a kind after the cursor (round to the start at the end).
function nextHint(key) {
  if (!view) return;
  const list = lastHints.hints.filter((h) => h.key === key);
  if (!list.length) return;
  const here = view.state.selection.to;
  const h = list.find((x) => x.from >= here) || list[0];
  const max = view.state.doc.content.size;
  view.dispatch(view.state.tr.setSelection(TextSelection.create(view.state.doc, Math.min(h.from, max), Math.min(h.to, max))).scrollIntoView());
  view.focus();
}

// ------------------------------------------- the chapter's opening (0.92)
// What the app sends with a chapter: {header: {blob, symbol, size, title, tone, alt}, date, pov: {id, name, colour, blob,
// symbol, initials}, dated: bool}. Shown above the text and never part of it: the date opens its When in the app, the
// viewpoint their bible page.
let chapterHead = null;
function setHead(h) {
  chapterHead = h && typeof h === "object" ? h : null;
  const top = document.getElementById("ctop"), pic = document.getElementById("cpic"), img = pic.querySelector("img");
  const orn = document.getElementById("corn"), meta = document.getElementById("cmeta"), dateLine = document.getElementById("cdate");
  top.className = "";
  pic.hidden = true;
  orn.hidden = true;
  meta.hidden = true;
  meta.replaceChildren();
  dateLine.hidden = true;
  dateLine.replaceChildren();
  if (!chapterHead) return;
  const hd = chapterHead.header;
  const pick = (v, list, d) => (list.includes(v) ? v : d);
  if (hd && typeof hd.blob === "string" && /^[0-9a-f]{64}$/.test(hd.blob)) {
    img.src = objectUrl(hd.blob);
    img.alt = hd.alt || "";
    pic.hidden = false;
    const placement = pick(hd.title, ["above", "below", "over"], "below");
    top.classList.add("size-" + pick(hd.size, ["full", "band", "ornament"], "band"), "title-" + placement);
    if (placement === "over") top.classList.add(hd.tone === "fade" ? "tone-fade" : "tone-dark");
  } else if (hd && typeof hd.symbol === "string" && hd.symbol) {
    orn.textContent = hd.symbol;
    orn.hidden = false;
    top.classList.add("title-" + (hd.title === "above" ? "above" : "below"));
  }
  const chips = [];
  const p = chapterHead.pov;
  if (p && typeof p.id === "string" && p.id) {
    const b = document.createElement("button");
    b.type = "button";
    b.className = "cchip pov";
    b.title = "Whose eyes it's seen through: open " + (p.name || "their") + "'s page in the bible";
    b.setAttribute("aria-label", "Told by " + (p.name || "a character") + ". Open their page in the bible");
    // The bible's icon, larger (4b, 0.102); the name beside it unless Aa turns it off.
    b.append(iconFor(p, p.name));
    const nm = document.createElement("span");
    nm.className = "pname";
    nm.textContent = p.name || "";
    b.append(nm);
    b.addEventListener("mousedown", (e) => e.preventDefault());
    b.addEventListener("click", () => { flush(); send({ type: "entry", id: p.id }); });
    chips.push(b);
  }
  // Its date as a line, written as the book prints it (0.105): the same text Typesetting, the ebook and Word use.
  if (chapterHead.date || chapterHead.dated === false) {
    dateLine.className = chapterHead.date ? "" : "empty";
    // In parts, each with its look (bold, italic, small capitals or upper case), aligned and placed as it prints (0.106).
    const partsLine = (parts) => {
      const line = document.createElement("span");
      line.className = "cline";
      for (const p of Array.isArray(parts) ? parts : []) {
        const s = document.createElement("span");
        s.textContent = String(p.t || "");
        s.style.fontWeight = p.b ? "700" : "400";
        s.style.fontStyle = p.i === false ? "normal" : "italic";
        if (p.c === "small") s.style.fontVariant = "small-caps";
        else if (p.c === "upper") s.style.textTransform = "uppercase";
        line.append(s);
      }
      return line;
    };
    if (chapterHead.date && Array.isArray(chapterHead.dateParts) && chapterHead.dateParts.length) {
      dateLine.append(partsLine(chapterHead.dateParts));
      if (Array.isArray(chapterHead.timeParts) && chapterHead.timeParts.length) {
        const t = partsLine(chapterHead.timeParts);
        t.classList.add("ctime");
        dateLine.append(t);
      }
    } else {
      dateLine.textContent = chapterHead.date || "Not on the timeline: give it a date";
      if (chapterHead.time) {
        const t = document.createElement("span");
        t.className = "ctime";
        t.textContent = chapterHead.time;
        dateLine.append(t);
      }
    }
    const align = ["left", "center", "right"].includes(chapterHead.dateAlign) ? chapterHead.dateAlign : "center";
    dateLine.style.textAlign = align;
    dateLine.style.alignSelf = align === "left" ? "flex-start" : align === "right" ? "flex-end" : "center";
    top.classList.add("date-" + (["under", "header", "above"].includes(chapterHead.datePlace) ? chapterHead.datePlace : "under"));
    dateLine.title = chapterHead.date ? "When it happens: click to change it" : "Give it a date or a day on the timeline";
    dateLine.onmousedown = (e) => e.preventDefault();
    dateLine.onclick = () => send({ type: "editWhen" });
    dateLine.hidden = false;
  }
  if (chips.length) { meta.append(...chips); meta.hidden = false; }
  requestAnimationFrame(() => placeCaret());   // the text moved down or up
  img.onload = () => placeCaret();
}

// The View panel: the "Aa" button in the toolbar.
function buildViewMenu() {
  const btn = document.getElementById("viewbtn"), menu = document.getElementById("viewmenu");
  const close = () => { menu.hidden = true; btn.setAttribute("aria-expanded", "false"); };
  btn.addEventListener("mousedown", (e) => {
    e.preventDefault();
    menu.hidden = !menu.hidden;
    btn.setAttribute("aria-expanded", menu.hidden ? "false" : "true");
    if (!menu.hidden) {
      const r = btn.getBoundingClientRect();
      menu.style.left = Math.max(8, Math.min(r.left, window.innerWidth - 352)) + "px";
      menu.style.top = (r.bottom + 6) + "px";
      syncViewMenu();
    }
  });
  document.addEventListener("mousedown", (e) => { if (!menu.hidden && !menu.contains(e.target) && e.target !== btn) close(); });
  document.addEventListener("keydown", (e) => { if (e.key === "Escape" && !menu.hidden) { close(); if (view) view.focus(); } });
  menu.querySelectorAll("[data-set]").forEach((el) => {
    el.addEventListener("click", () => {
      const key = el.dataset.set;
      let val = el.dataset.value;
      if (key === "size") val = +val;
      changeView({ [key]: val });
    });
  });
  menu.querySelectorAll("input[type=checkbox]").forEach((el) => {
    el.addEventListener("change", () => changeView({ [el.name]: el.checked }));
  });
  for (const [id, dir] of [["vm-zoomout", -1], ["vm-zoomin", 1], ["vm-zoomreset", 0]]) {
    const b = document.getElementById(id);
    if (b) b.addEventListener("click", () => zoomPage(dir));
  }
  // Ctrl and the wheel over the page zoom the page, not the window (the app leaves WebView2's own zoom off).
  const scroller = document.getElementById("scroller");
  if (scroller)
    scroller.addEventListener("wheel", (e) => {
      if (!e.ctrlKey) return;
      e.preventDefault();
      if (e.deltaY) zoomPage(e.deltaY < 0 ? 1 : -1);
    }, { passive: false });
}

// The page's zoom: Ctrl and + / - / 0 while writing, Ctrl and the wheel over the page, or Aa's − and + (0.97).
function zoomPage(dir) {
  const z = stepZoom(viewSettings.zoom, dir);
  if (Math.abs(z - clampZoom(viewSettings.zoom)) < 0.001 && dir !== 0) return true;
  changeView({ zoom: z });
  notice(`Page ${Math.round(z * 100)}%. Ctrl+0 goes back to 100%.`);
  return true;
}

function syncViewMenu() {
  const menu = document.getElementById("viewmenu");
  if (!menu) return;
  const zl = document.getElementById("vm-zoom");
  if (zl) zl.textContent = Math.round(clampZoom(viewSettings.zoom) * 100) + "%";
  menu.querySelectorAll("[data-set]").forEach((el) => {
    const v = viewSettings[el.dataset.set];
    el.setAttribute("aria-pressed", String(v) === el.dataset.value ? "true" : "false");
  });
  menu.querySelectorAll("input[type=checkbox]").forEach((el) => { el.checked = !!viewSettings[el.name]; });
  document.getElementById("viewbtn").setAttribute("aria-pressed", viewSettings.focus || viewSettings.typewriter ? "true" : "false");
}

// ------------------------------------------------------- world typefaces (0.41)
// The world's own fonts, sent by the app: [{id, name, faces: {regular, italic, bold, boldItalic}}] (URLs on the
// library's font host). Each is loaded with the FontFace API (the page's CSP keeps out inline styles, not this) and
// given a class rule, so words marked with it show in it as you write.
let worldFonts = [];
const loadedFonts = new Set();

function setWorldFonts(list) {
  worldFonts = (Array.isArray(list) ? list : []).filter((f) => f && /^[a-z0-9-]{1,64}$/.test(f.id));
  const sheet = [...document.styleSheets].find((s) => { try { return !!s.cssRules; } catch { return false; } });
  for (const f of worldFonts) {
    const key = f.id + "|" + JSON.stringify(f.faces || {});
    if (loadedFonts.has(key)) continue;
    loadedFonts.add(key);
    const family = "NR F " + f.id;
    const faces = f.faces || {};
    for (const [k, weight, style] of [["regular", "400", "normal"], ["italic", "400", "italic"], ["bold", "700", "normal"], ["boldItalic", "700", "italic"]]) {
      const url = faces[k];
      if (typeof url !== "string" || !url.startsWith("https://worldfonts.nrstudio.localhost/")) continue;
      try {
        const face = new FontFace(family, `url("${url}")`, { weight, style });
        document.fonts.add(face);
        face.load().catch(() => {});
      } catch {}
    }
    if (sheet) { try { sheet.insertRule(`.ff-${f.id} { font-family: "${family}", var(--text-font, serif); }`, sheet.cssRules.length); } catch {} }
  }
  if (view) updateToolbar(view.state);
}
let bookFontName = "";   // the book's own typeface (from its Typesetting design), sent by the app

// Sets the selection (or what's typed next) in a world typeface, or back in the book's own (id "").
function setWordsFont(id) {
  if (!view) return;
  const { state } = view;
  const { from, to, empty } = state.selection;
  let tr = state.tr;
  if (empty) {
    tr = id ? tr.addStoredMark(M.font.create({ family: id })) : tr.removeStoredMark(M.font);
  } else {
    tr = tr.removeMark(from, to, M.font);
    if (id) tr = tr.addMark(from, to, M.font.create({ family: id }));
  }
  view.dispatch(tr);
  view.focus();
}

// The typeface of the selection: a world font's id, "" for the book's own, or null when it's mixed.
// The chapter list under the chapter's name (0.49): every part, chapter and scene of the book.
let chapters = [];
function buildChapterMenu() {
  const btn = document.getElementById("chapname"), menu = document.getElementById("chapmenu");
  if (!btn || !menu) return;
  const close = () => { menu.hidden = true; btn.setAttribute("aria-expanded", "false"); };
  btn.addEventListener("mousedown", (e) => {
    e.preventDefault();
    if (!menu.hidden) { close(); return; }
    menu.replaceChildren();
    if (!chapters.length) { const n = document.createElement("div"); n.className = "fm-empty"; n.textContent = "No chapters yet."; menu.append(n); }
    for (const c of chapters) {
      const b = document.createElement("button");
      b.type = "button";
      b.className = "cm-item " + (c.kind || "chapter");
      b.style.paddingLeft = (10 + 16 * Math.min(4, c.depth | 0)) + "px";
      const t = document.createElement("span");
      t.textContent = c.title || (c.kind === "scene" ? "Untitled scene" : c.kind === "part" ? "Untitled part" : "Untitled chapter");
      b.append(t);
      if (c.kind !== "part" && c.words > 0) { const w = document.createElement("span"); w.className = "cm-w"; w.textContent = Number(c.words).toLocaleString() + " words"; b.append(w); }
      if (c.id === currentId) b.setAttribute("aria-current", "true");
      b.addEventListener("mousedown", (ev) => { ev.preventDefault(); close(); if (c.id !== currentId) { flush(); send({ type: "openNode", id: c.id }); } });
      menu.append(b);
    }
    menu.hidden = false;
    btn.setAttribute("aria-expanded", "true");
    const r = btn.getBoundingClientRect();
    menu.style.left = Math.max(8, Math.min(r.left - 60, window.innerWidth - 330)) + "px";
    menu.style.top = (r.bottom + 4) + "px";
    const on = menu.querySelector('[aria-current="true"]');
    if (on) on.scrollIntoView({ block: "center" });
  });
  document.addEventListener("mousedown", (e) => { if (!menu.hidden && !menu.contains(e.target) && e.target !== btn && !btn.contains(e.target)) close(); });
  document.addEventListener("keydown", (e) => { if (e.key === "Escape" && !menu.hidden) { close(); if (view) view.focus(); } });
}

function setChapName(t) {
  const el = document.getElementById("chapname");
  if (!el) return;
  el.textContent = t || "";
  el.title = t || "";
  fitBar();
}

function selectionFont(state) {
  const { $from, from, to, empty } = state.selection;
  if (empty) {
    const m = (state.storedMarks || $from.marks()).find((x) => x.type === M.font);
    return m ? m.attrs.family : "";
  }
  let found;
  state.doc.nodesBetween(from, to, (n) => {
    if (!n.isText) return;
    const m = n.marks.find((x) => x.type === M.font);
    const f = m ? m.attrs.family : "";
    if (found === undefined) found = f;
    else if (found !== f) found = null;
  });
  return found === undefined ? "" : found;
}

// The typeface box shows the selection's font by name, in that font (like a word processor's).
function updateFontBox(state) {
  const el = document.getElementById("fontname");
  if (!el) return;
  const f = selectionFont(state);
  el.className = "";
  if (f === null) { el.textContent = "Several typefaces"; el.classList.add("book"); return; }
  if (!f) { el.textContent = bookFontName ? bookFontName : "Book typeface"; el.classList.add("book"); return; }
  const wf = worldFonts.find((x) => x.id === f);
  el.textContent = wf ? String(wf.name || wf.id) : f + " (not in this world)";
  if (wf) el.classList.add("ff-" + f);
}

function buildFontMenu() {
  const btn = document.getElementById("fontbtn"), menu = document.getElementById("fontmenu");
  if (!btn || !menu) return;
  const close = () => { menu.hidden = true; btn.setAttribute("aria-expanded", "false"); };
  btn.addEventListener("mousedown", (e) => {
    e.preventDefault();
    if (!menu.hidden) { close(); return; }
    const cur = view ? selectionFont(view.state) : "";
    menu.replaceChildren();
    const head = (t) => { const h = document.createElement("div"); h.className = "mm-head"; h.textContent = t; menu.append(h); };
    const item = (id, label, cls, note) => {
      const b = document.createElement("button");
      b.type = "button";
      b.className = "fm-item" + (cls ? " " + cls : "");
      const l = document.createElement("span");
      l.textContent = label;
      b.append(l);
      if (note) { const n = document.createElement("span"); n.className = "fm-note"; n.textContent = note; b.append(n); }
      b.setAttribute("aria-pressed", id === cur ? "true" : "false");
      b.addEventListener("mousedown", (ev) => { ev.preventDefault(); close(); setWordsFont(id); });
      menu.append(b);
    };
    head("This book");
    item("", bookFontName ? bookFontName : "Book typeface", "book", "the book's own");
    head("This world's fonts");
    const own = worldFonts.filter((f) => !f.builtIn), built = worldFonts.filter((f) => f.builtIn);
    if (!own.length) {
      const n = document.createElement("div");
      n.className = "fm-empty";
      n.textContent = "None of its own yet. Add a world's runes, symbols or handwriting, or use the built-in ones below.";
      menu.append(n);
    }
    for (const f of own) item(f.id, String(f.name || f.id), "ff-" + f.id);
    if (built.length) {
      head("Built in");
      for (const f of built) item(f.id, String(f.name || f.id), "ff-" + f.id);
    }
    const rule = document.createElement("div");
    rule.className = "fm-rule";
    menu.append(rule);
    const add = document.createElement("button");
    add.type = "button";
    add.className = "fm-add";
    add.textContent = "Add fonts to this world…";
    add.addEventListener("mousedown", (ev) => { ev.preventDefault(); close(); send({ type: "openFonts" }); });
    menu.append(add);
    menu.hidden = false;
    btn.setAttribute("aria-expanded", "true");
    // Under the box, or under More when the box is folded away.
    const anchor = btn.offsetParent ? btn : document.getElementById("morebtn");
    const r = anchor.getBoundingClientRect();
    menu.style.left = Math.max(8, Math.min(r.left, window.innerWidth - 310)) + "px";
    menu.style.top = (r.bottom + 4) + "px";
    const on = menu.querySelector('[aria-pressed="true"]');
    if (on) on.focus({ preventScroll: true });
  });
  document.addEventListener("mousedown", (e) => { if (!menu.hidden && !menu.contains(e.target) && e.target !== btn && !btn.contains(e.target)) close(); });
  document.addEventListener("keydown", (e) => { if (e.key === "Escape" && !menu.hidden) { close(); if (view) view.focus(); } });
}

// ------------------------------------------------- the toolbar on narrow windows (0.42)
// Groups fold away, least-used first, until the bar fits; More lists every folded tool by name, with its shortcut.
function fitBar() {
  const bar = document.getElementById("bar"), more = document.getElementById("morebtn");
  if (!bar || !more) return;
  const groups = [...bar.querySelectorAll(".grp")];
  groups.forEach((g) => g.classList.remove("folded"));
  more.hidden = true;
  if (bar.scrollWidth <= bar.clientWidth + 1) return;
  more.hidden = false;
  const order = groups.filter((g) => +g.dataset.prio < 9).sort((a, b) => (+a.dataset.prio - +b.dataset.prio) || (groups.indexOf(b) - groups.indexOf(a)));
  for (const g of order) {
    if (bar.scrollWidth <= bar.clientWidth + 1) break;
    g.classList.add("folded");
  }
}

function buildMoreMenu() {
  const btn = document.getElementById("morebtn"), menu = document.getElementById("moremenu");
  if (!btn || !menu) return;
  const close = () => { menu.hidden = true; btn.setAttribute("aria-expanded", "false"); };
  btn.addEventListener("mousedown", (e) => {
    e.preventDefault();
    if (!menu.hidden) { close(); return; }
    menu.replaceChildren();
    for (const g of document.querySelectorAll("#bar .grp.folded")) {
      const h = document.createElement("div");
      h.className = "mm-head";
      h.textContent = g.dataset.label || "";
      menu.append(h);
      for (const src of g.querySelectorAll("button")) {
        if (src.hidden) continue;
        const b = document.createElement("button");
        b.type = "button";
        b.className = "mm-item";
        b.disabled = src.disabled;
        const ico = document.createElement("span");
        ico.className = "mm-ico";
        if (src.id === "hints" || src.id === "fontbtn") ico.textContent = src.id === "hints" ? "!" : "Ff";
        else ico.innerHTML = src.innerHTML;   // the button's own icon (markup from this page)
        const lab = document.createElement("span");
        lab.className = "mm-label";
        lab.textContent = src.id === "hints" ? "Style hints: " + src.textContent : (src.dataset.label || src.getAttribute("aria-label") || "");
        const key = document.createElement("span");
        key.className = "mm-key";
        key.textContent = src.dataset.key || "";
        b.append(ico, lab, key);
        if (src.getAttribute("aria-pressed") === "true") b.setAttribute("aria-pressed", "true");
        b.addEventListener("mousedown", (ev) => {
          ev.preventDefault();
          close();
          src.dispatchEvent(new MouseEvent("mousedown", { bubbles: true, cancelable: true }));
        });
        menu.append(b);
      }
    }
    menu.hidden = false;
    btn.setAttribute("aria-expanded", "true");
    const r = btn.getBoundingClientRect();
    menu.style.left = Math.max(8, Math.min(r.left, window.innerWidth - 280)) + "px";
    menu.style.top = (r.bottom + 4) + "px";
  });
  document.addEventListener("mousedown", (e) => { if (!menu.hidden && !menu.contains(e.target) && !btn.contains(e.target)) close(); });
  document.addEventListener("keydown", (e) => { if (e.key === "Escape" && !menu.hidden) { close(); if (view) view.focus(); } });
  new ResizeObserver(() => fitBar()).observe(document.getElementById("bar"));
  window.addEventListener("resize", fitBar);
}

const toggleFocus = () => { changeView({ focus: !viewSettings.focus }); return true; };
const toggleTypewriter = () => { changeView({ typewriter: !viewSettings.typewriter }); return true; };

// ------------------------------------------------------------------- state

let view = null;
let currentId = null;
let dirty = false;
let timer = null;
let readOnly = false;

let placeholderText = "";

function flush() {
  clearTimeout(timer);
  timer = null;
  if (!view || !currentId || !dirty) return;
  dirty = false;
  // "Saved" appears when the app confirms it has stored the text.
  const docJSON = view.state.doc.toJSON();
  send({ type: "change", id: currentId, doc: docJSON, words: docWords(view.state.doc) });
  rememberVersion(currentId, docJSON, false);
}

// "Saving..." while there are changes not yet stored, then "Saved".
// "Saved just now", then "Saved a moment ago", "Saved 5 minutes ago"... kept up to date (0.35).
let savedAt = 0, savedState = "saved", savedTicker = null;
function savedText() {
  // While you type, the last save is still what it says, so the word doesn't flicker on every pause (0.49).
  if (savedState === "saving" && !savedAt) return "Saving\u2026";
  if (savedState === "failed") return "Not saved";
  if (!savedAt) return "Saved";
  const s = (Date.now() - savedAt) / 1000;
  if (s < 10) return "Saved just now";
  if (s < 60) return "Saved a moment ago";
  const m = Math.floor(s / 60);
  if (m < 60) return m === 1 ? "Saved a minute ago" : `Saved ${m} minutes ago`;
  return "Saved at " + new Date(savedAt).toLocaleTimeString(document.documentElement.lang || undefined, { hour: "numeric", minute: "2-digit" });
}
function setSaved(state, fresh) {
  const el = document.getElementById("saved");
  if (!el) return;
  savedState = state;
  if (state === "saved" && fresh) savedAt = Date.now();
  if (state === "saved" && fresh === false) savedAt = 0;
  el.textContent = savedText();
  el.dataset.state = state;
  el.title = savedAt ? "Saved at " + new Date(savedAt).toLocaleTimeString() : "";
  if (!savedTicker) savedTicker = setInterval(() => { const x = document.getElementById("saved"); if (x) x.textContent = savedText(); }, 10000);
}

// --------------------------------------------------------------- writing sprints (0.49.1)
// A timed burst: the words added in it are counted across chapters (each chapter's words when the sprint reached it,
// against its words now), the bar counts down, and a note says how it went. Nothing is kept afterwards.
let sprint = null;   // { end, minutes, done, base, chapter }
function sprintWords() {
  if (!sprint || !view) return 0;
  return Math.max(0, sprint.done + (docWords(view.state.doc) - sprint.base));
}
function startSprint(minutes) {
  sprint = { end: Date.now() + minutes * 60000, minutes, done: 0, base: view ? docWords(view.state.doc) : 0, chapter: currentId };
  sentSprintWords = -1;
  tickSprint();
  if (view) view.focus();
}
function stopSprint(finished) {
  if (!sprint) return;
  const words = sprintWords(), mins = finished ? sprint.minutes : Math.max(1, Math.round((Date.now() - (sprint.end - sprint.minutes * 60000)) / 60000));
  sprint = null;
  clearTimeout(sprintTimer);
  // The app passes sprints on to NorthRay Capture's sprint overlay (0.53).
  send({ type: "sprint", running: false, finished: !!finished, words, minutes: mins });
  notice(finished ? `Sprint done: ${words.toLocaleString()} ${words === 1 ? "word" : "words"} in ${mins} minutes.`
                  : `Sprint stopped: ${words.toLocaleString()} ${words === 1 ? "word" : "words"} in ${mins} ${mins === 1 ? "minute" : "minutes"}.`);
  stats();
}
let sprintTimer = null;
let sentSprintWords = -1;
function tickSprint() {
  clearTimeout(sprintTimer);
  if (!sprint) return;
  if (Date.now() >= sprint.end) { stopSprint(true); return; }
  stats();
  const w = sprintWords();
  if (w !== sentSprintWords) {
    sentSprintWords = w;
    send({ type: "sprint", running: true, end: sprint.end, minutes: sprint.minutes, words: w });
  }
  sprintTimer = setTimeout(tickSprint, 1000);
}
// A chapter changing mid-sprint: what was written in the last one is kept, and the new one starts from its words now.
function sprintChapterChanged() {
  if (!sprint) return;
  sprint.done = sprintWords();   // the base is set again once the new chapter is in
}
function buildSprintMenu() {
  const btn = document.getElementById("count"), menu = document.getElementById("sprintmenu");
  if (!btn || !menu) return;
  const close = () => { menu.hidden = true; btn.setAttribute("aria-expanded", "false"); };
  btn.addEventListener("mousedown", (e) => {
    e.preventDefault();
    if (!menu.hidden) { close(); return; }
    menu.replaceChildren();
    const head = document.createElement("div");
    head.className = "mm-head";
    head.textContent = sprint ? "Sprint" : "Start a writing sprint";
    menu.append(head);
    const note = document.createElement("div");
    note.className = "sp-note";
    note.textContent = sprint ? `${sprintWords().toLocaleString()} words so far.` : "A timed burst of writing: the bar counts down and adds up the words you write, in any chapter.";
    menu.append(note);
    const item = (label, fn) => {
      const b = document.createElement("button");
      b.type = "button";
      b.className = "cm-item";
      b.textContent = label;
      b.addEventListener("mousedown", (ev) => { ev.preventDefault(); close(); fn(); });
      menu.append(b);
    };
    if (sprint) item("Stop the sprint", () => stopSprint(false));
    else for (const m of [10, 15, 25, 45]) item(`${m} minutes`, () => startSprint(m));
    menu.hidden = false;
    btn.setAttribute("aria-expanded", "true");
    const r = btn.getBoundingClientRect();
    menu.style.left = Math.max(8, Math.min(r.right - 250, window.innerWidth - 258)) + "px";
    menu.style.top = (r.bottom + 4) + "px";
  });
  document.addEventListener("mousedown", (e) => { if (!menu.hidden && !menu.contains(e.target) && e.target !== btn && !btn.contains(e.target)) close(); });
  document.addEventListener("keydown", (e) => { if (e.key === "Escape" && !menu.hidden) { close(); if (view) view.focus(); } });
}

function stats() {
  if (!view) return;
  const { from, to } = view.state.selection;
  const sel = from === to ? 0 : countWords(view.state.doc.textBetween(from, to, " ", " "));
  const words = docWords(view.state.doc);
  const countEl = document.getElementById("count");
  if (sprint) {
    const left = Math.max(0, Math.ceil((sprint.end - Date.now()) / 1000));
    const mm = Math.floor(left / 60), ss = String(left % 60).padStart(2, "0");
    countEl.textContent = `${mm}:${ss} \u00B7 +${sprintWords().toLocaleString()} words`;
    countEl.classList.add("sprinting");
    countEl.title = "A writing sprint: time left and the words written in it. Click to stop it.";
  } else {
    countEl.textContent = (sel ? sel.toLocaleString() + " of " : "") + words.toLocaleString() + (words === 1 ? " word" : " words");
    countEl.classList.remove("sprinting");
    countEl.title = "Words in this chapter. Click to start a writing sprint.";
  }
  const h = document.getElementById("hints");
  if (h) { h.textContent = hintCount === 1 ? "1 hint" : hintCount + " hints"; h.dataset.some = hintCount > 0 ? "true" : "false"; }
  send({ type: "stats", id: currentId, words, selectionWords: sel });
}

// Words to show (Review's "Show it", Worldbuilding's Appears in): every use in the chapter is
// highlighted, the first selected and scrolled to the middle. The highlight goes at the next edit.
const foundLook = new Plugin({
  state: {
    init: () => DecorationSet.empty,
    apply(tr, set) {
      const found = tr.getMeta("found");
      if (found) return found;
      return tr.docChanged ? DecorationSet.empty : set;
    },
  },
  props: { decorations(state) { return this.getState(state); } },
});

function findWords(words) {
  if (!view || !Array.isArray(words) || !words.length) return;
  const terms = [...new Set(words.map((x) => String(x).trim()).filter((x) => x.length >= 2))].sort((a, b) => b.length - a.length);
  if (!terms.length) return;
  const esc = terms.map((t) => t.replace(/[.*+?^${}()|[\]\\]/g, "\\$&"));
  const re = new RegExp("(?<![\\p{L}\\p{N}])(?:" + esc.join("|") + ")(?![\\p{L}\\p{N}])", "giu");
  const ranges = [];
  for (const b of textBlocks(view.state.doc))
    for (const m of b.text.matchAll(re)) ranges.push({ from: b.pos + m.index, to: b.pos + m.index + m[0].length });
  if (!ranges.length) { notice(`"${terms[0]}" isn't in this chapter any more.`); return; }
  const decos = ranges.map((r, i) => Decoration.inline(r.from, r.to, { class: i === 0 ? "found found-first" : "found" }));
  const tr = view.state.tr.setMeta("found", DecorationSet.create(view.state.doc, decos))
    .setSelection(TextSelection.create(view.state.doc, ranges[0].from, ranges[0].to));
  view.dispatch(tr);
  // Into the middle of the window, not just the edge.
  requestAnimationFrame(() => {
    const at = view.coordsAtPos(ranges[0].from), sc = document.getElementById("scroller");
    if (sc && at) sc.scrollTop += at.top - sc.getBoundingClientRect().top - sc.clientHeight * 0.4;
  });
  if (ranges.length > 1) notice(`${ranges.length} uses of "${terms[0]}" in this chapter are highlighted.`);
}

const watcher = new Plugin({
  view: () => ({
    update: (v, prev) => {
      if (!v.state.doc.eq(prev.doc) && !readOnly) {
        if (!dirty) send({ type: "saved-state", dirty: true });
        dirty = true;
        setSaved("saving");
        clearTimeout(timer);
        timer = setTimeout(flush, 800);
      }
      if (!v.state.doc.eq(prev.doc) || !v.state.selection.eq(prev.selection)) {
        stats(); updateToolbar(v.state); reportPicture(v.state); reportComment(v.state); keepCaretCentred(); placeCaret();
      }
    },
  }),
});

// An empty chapter shows a faint prompt where the text will go.
const placeholder = new Plugin({
  props: {
    decorations(state) {
      const d = state.doc;
      if (readOnly || !placeholderText || d.childCount !== 1 || d.firstChild.type !== S.paragraph || d.firstChild.content.size) return null;
      return DecorationSet.create(d, [Decoration.node(0, d.firstChild.nodeSize, { class: "placeholder", "data-placeholder": placeholderText })]);
    },
  },
});

// Tell the app when a picture is selected (or no longer is), so it can show its details.
let reported = "";
function reportPicture(state) {
  const sel = state.selection;
  const node = sel.node && sel.node.type === S.image ? sel.node : null;
  const key = node ? sel.from + ":" + JSON.stringify(node.attrs) : "";
  if (key === reported) return;
  reported = key;
  send(node ? { type: "picture", id: currentId, pos: sel.from, attrs: node.attrs } : { type: "picture", id: currentId, pos: null });
}

// The app changes a picture's placement, caption or description, or removes it.
function setPicture(pos, attrs) {
  if (!view || readOnly) return;
  const node = view.state.doc.nodeAt(pos);
  if (!node || node.type !== S.image || (attrs.blob && attrs.blob !== node.attrs.blob)) return;
  const next = { ...node.attrs };
  for (const k of ["alt", "caption", "cropX", "cropY", "cropW", "cropH", "width", "align"]) if (k in attrs) next[k] = attrs[k];
  const tr = view.state.tr.setNodeMarkup(pos, null, next);
  tr.setSelection(NodeSelection.create(tr.doc, pos));
  view.dispatch(tr);
}
function removePicture(pos) {
  if (!view || readOnly) return;
  const node = view.state.doc.nodeAt(pos);
  if (!node || node.type !== S.image) return;
  view.dispatch(view.state.tr.delete(pos, pos + node.nodeSize).scrollIntoView());
  view.focus();
}

// ------------------------------------------------- kept in this computer's editor storage (0.35)
// Where each chapter was scrolled to, and earlier versions of it for undo after reopening the book. Both are
// conveniences: if the storage is full or unavailable, the editor works as before.
const store = {
  get(key, fallback) { try { const v = localStorage.getItem(key); return v ? JSON.parse(v) : fallback; } catch (e) { return fallback; } },
  set(key, value) {
    const text = JSON.stringify(value);
    for (let tries = 0; tries < 6; tries++) {
      try { localStorage.setItem(key, text); return true; }
      catch (e) {   // full: let go of the chapter histories used longest ago
        const index = store.get(UNDO_INDEX, []);
        const old = index.shift();
        if (!old) return false;
        try { localStorage.removeItem(UNDO_PREFIX + old); localStorage.setItem(UNDO_INDEX, JSON.stringify(index)); } catch (e2) { return false; }
      }
    }
    return false;
  },
};
const SCROLLS = "nr.scroll";
function rememberScroll() {
  const sc = document.getElementById("scroller");
  if (!currentId || !sc) return;
  const all = store.get(SCROLLS, {});
  all[currentId] = Math.round(sc.scrollTop);
  const keys = Object.keys(all);
  if (keys.length > 3000) for (const k of keys.slice(0, keys.length - 3000)) delete all[k];
  store.set(SCROLLS, all);
}
let scrollSaveTimer = null;
document.getElementById("scroller").addEventListener("scroll", () => {
  clearTimeout(scrollSaveTimer);
  scrollSaveTimer = setTimeout(rememberScroll, 400);
});

// Undo after reopening: a version of the chapter is kept now and then (at most every 90 seconds while writing,
// and on leaving it); opening it again replays them into the undo history, so Ctrl+Z steps back through them.
const UNDO_PREFIX = "nr.undo.", UNDO_INDEX = "nr.undo.index", UNDO_KEEP = 12, UNDO_EVERY = 90000;
const lastVersionAt = new Map();
function rememberVersion(id, docJSON, force) {
  if (!id || readOnly) return;
  const now = Date.now();
  if (!force && now - (lastVersionAt.get(id) || 0) < UNDO_EVERY) return;
  lastVersionAt.set(id, now);
  const list = store.get(UNDO_PREFIX + id, []);
  const text = JSON.stringify(docJSON);
  if (list.length && JSON.stringify(list[list.length - 1]) === text) return;
  list.push(docJSON);
  while (list.length > UNDO_KEEP) list.shift();
  while (list.length > 1 && JSON.stringify(list).length > 700000) list.shift();
  const index = store.get(UNDO_INDEX, []).filter((x) => x !== id);
  index.push(id);
  store.set(UNDO_INDEX, index);
  store.set(UNDO_PREFIX + id, list);
}

function parseDoc(json) {
  try { const d = schema.nodeFromJSON(json); d.check(); return d; } catch (e) { return null; }
}

function makeState(docJSON, id) {
  let doc = parseDoc(docJSON);
  if (!doc) doc = schema.node("doc", null, [schema.node("paragraph")]);
  const plugins = [rules, keymap(keys), keymap(baseKeymap), history({ depth: 200 }), dropCursor({ color: "var(--accent)" }), gapCursor(), watcher, placeholder, commentLook, focusLook, hereLook, lookups, foundLook, comicLook];
  // Earlier versions kept from before: start from the oldest and step forward to the text as it is now.
  const older = (id ? store.get(UNDO_PREFIX + id, []) : []).map(parseDoc).filter(Boolean).filter((d) => !d.eq(doc));
  if (!older.length) return EditorState.create({ doc, plugins });
  let state = EditorState.create({ doc: older[0], plugins });
  for (const next of [...older.slice(1), doc]) {
    if (next.eq(state.doc)) continue;
    const tr = state.tr.replaceWith(0, state.doc.content.size, next.content);
    tr.setMeta("restoringHistory", true);
    state = state.apply(closeHistory(tr));
  }
  return state.apply(state.tr.setSelection(TextSelection.atStart(state.doc)));
}

function load(msg) {
  flush();
  if (view && currentId) { rememberScroll(); rememberVersion(currentId, view.state.doc.toJSON(), true); }
  if (typeof msg.dark === "boolean") setTheme(msg.dark);
  if (sprint) sprintChapterChanged();   // keep what the last chapter added
  currentId = msg.id;
  readOnly = !!msg.readOnly;
  placeholderText = msg.placeholder || "";
  document.documentElement.lang = msg.language || "en-GB";
  // The dictionary for the book's language (loaded once; the text is checked again when it's ready).
  spell.load(document.documentElement.lang, (ok) => { if (ok && view) { applyView(); rehintNow(); } });
  document.getElementById("title").textContent = msg.title || "";
  setChapName(msg.title);
  setHead(msg.head);
  if (Array.isArray(msg.chapters)) chapters = msg.chapters;
  const state = makeState(msg.doc, readOnly ? null : msg.id);
  if (!readOnly) rememberVersion(msg.id, state.doc.toJSON(), true);   // how it was when opened, to undo back to later
  if (!view) {
    view = new EditorView(document.getElementById("page"), {
      state,
      editable: () => !readOnly,
      attributes: { spellcheck: viewSettings.spellcheck ? "true" : "false", "aria-label": "Manuscript text" },
      nodeViews: { image: (node) => new ImageView(node) },
      handleDOMEvents: {
        blur: () => { flush(); requestAnimationFrame(placeCaret); return false; },
        focus: () => { requestAnimationFrame(placeCaret); return false; },
        compositionstart: () => { placeCaret(true); return false; },
        compositionend: () => { requestAnimationFrame(() => placeCaret()); return false; },
        click: (v, e) => {
          const a = e.target.closest && e.target.closest("a[href]");
          if (a && (e.ctrlKey || e.metaKey)) { send({ type: "link", href: a.getAttribute("href") }); e.preventDefault(); return true; }
          const n = e.target.closest && e.target.closest(".wl[data-entry]");
          if (n && (e.ctrlKey || e.metaKey)) {
            const ids = n.dataset.entry.split(" ");
            if (ids.length > 1) showNameCard(n);   // shared by several: choose on the card
            else { flush(); send({ type: "entry", id: ids[0] }); }
            e.preventDefault();
            return true;
          }
          return false;
        },
      },
      // Pasting (0.37): see paste.js.
      transformPastedHTML: cleanPastedHTML,
      clipboardTextParser: (text) => textToSlice(text, schema),
      transformPasted: (slice, v) => {
        const from = v.state.selection.from;
        const before = from > 1 ? v.state.doc.textBetween(Math.max(0, from - 1), from, "", "\uFFFC") : "";
        return tidySlice(slice, schema, before);
      },
      handlePaste: (v, e) => pastePicture(e),
    });
  } else {
    view.updateState(state);
    if (sprint) sprint.base = docWords(view.state.doc);   // the new chapter's words count from here
  }
  dirty = false;
  setSaved("saved", false);
  stats();
  updateToolbar(view.state);
  // Jump (not scroll) to where this chapter was left (0.35), so switching chapters doesn't flicker past the old one.
  const scroller = document.getElementById("scroller");
  scroller.style.scrollBehavior = "auto";
  const keptScroll = store.get(SCROLLS, {})[msg.id] || 0;
  scroller.scrollTop = keptScroll;
  requestAnimationFrame(() => { scroller.scrollTop = keptScroll; scroller.style.scrollBehavior = ""; placeCaret(); });
  if (!readOnly && msg.focus !== false) view.focus();
  reported = null;
  reportPicture(view.state);
  activeComment = null;
  reportedComment = null;
  reportComment(view.state);
}

// A cursor that glides to where you type and breathes rather than blinks (0.35). It's a line drawn over the text;
// Windows' own caret is hidden only while this one is shown (not while selecting, in IME composition or read-only).
function placeCaret(composing) {
  const c = document.getElementById("caret");
  if (!c) return;
  const root = document.documentElement;
  const sel = view && view.state.selection;
  const show = view && !readOnly && !composing && !view.composing && view.hasFocus() && sel && sel.empty && !(sel instanceof NodeSelection);
  if (!show) { c.hidden = true; root.classList.remove("smooth-caret"); return; }
  let r;
  try { r = view.coordsAtPos(sel.head); } catch (e) { c.hidden = true; root.classList.remove("smooth-caret"); return; }
  const a = c.parentElement.getBoundingClientRect();
  const first = c.hidden;
  c.hidden = false;
  root.classList.add("smooth-caret");
  c.classList.toggle("no-glide", first);
  c.style.height = Math.max(8, r.bottom - r.top) + "px";
  c.style.transform = `translate(${(r.left - a.left).toFixed(1)}px, ${(r.top - a.top).toFixed(1)}px)`;
  c.classList.remove("breathe");
  void c.offsetWidth;   // start the breathing again after each move
  c.classList.add("breathe");
}
window.addEventListener("resize", () => placeCaret());

function setTheme(dark) { document.documentElement.dataset.theme = dark ? "dark" : "light"; }

// ------------------------------------------------------------------ toolbar

function run(cmd) { if (view) { cmd(view.state, view.dispatch, view); view.focus(); } }

function updateToolbar(state) {
  const { $from, from, to, empty } = state.selection;
  const block = $from.parent;
  const sel = document.getElementById("style");
  sel.value = block.type === S.heading ? "h" + block.attrs.level : block.type === S.paragraph ? block.attrs.style : "";
  const active = (mark) => (empty ? !!mark.isInSet(state.storedMarks || $from.marks()) : state.doc.rangeHasMark(from, to, mark));
  for (const [id, mark] of [["b", M.strong], ["i", M.em], ["u", M.underline], ["sc", M.smallcaps]])
    document.getElementById(id).setAttribute("aria-pressed", active(mark) ? "true" : "false");
  document.getElementById("comment").disabled = empty || readOnly;
  updateFontBox(state);
}

function buildToolbar() {
  const sel = document.getElementById("style");
  for (const [id, label] of STYLES) sel.add(new Option(label, id));
  for (const l of [1, 2, 3]) sel.add(new Option("Heading " + l, "h" + l));
  const comic = document.createElement("optgroup");
  comic.label = "Comic script";
  comic.id = "comicstyles";
  comic.hidden = true;
  for (const [id, label] of COMIC_STYLES) comic.append(new Option(label, id));
  sel.add(comic);
  sel.addEventListener("change", () => {
    const v = sel.value;
    run(v.startsWith("h") ? setBlockType(S.heading, { level: +v.slice(1) }) : setStyle(v));
  });
  const bind = (id, cmd) => document.getElementById(id).addEventListener("mousedown", (e) => { e.preventDefault(); run(cmd); });
  bind("b", toggleMark(M.strong));
  bind("i", toggleMark(M.em));
  bind("u", toggleMark(M.underline));
  bind("sc", toggleMark(M.smallcaps));
  bind("break", insertSceneBreak);
  bind("quote", wrapIn(S.blockquote));
  bind("ul", wrapInList(S.bullet_list));
  bind("ol", wrapInList(S.ordered_list));
  document.getElementById("thes").addEventListener("mousedown", (e) => { e.preventDefault(); otherWords(); });
  bind("undo", undo);
  bind("redo", redo);
  bind("comment", addComment);
  bind("link", toggleLink);
  for (const [id, delta] of [["prev", -1], ["next", 1]])
    document.getElementById(id).addEventListener("mousedown", (e) => { e.preventDefault(); navigate(delta)(); });
  document.getElementById("img").addEventListener("mousedown", (e) => { e.preventDefault(); send({ type: "insertImage" }); });
}

// A picture pasted (a screenshot, an image copied from a page or a folder): kept in the artwork library by the app,
// which then puts it here as if inserted (0.37). Not when the clipboard has text too (a web page's words and pictures).
function pastePicture(e) {
  const cd = e.clipboardData;
  if (!cd || readOnly) return false;
  if (cd.getData("text/plain") || cd.getData("text/html")) return false;
  const file = Array.from(cd.files || []).find((f) => /^image\/(png|jpeg|gif|webp|bmp)$/.test(f.type));
  if (!file) return false;
  if (file.size > 40 * 1024 * 1024) { send({ type: "pasteImage", tooLarge: true }); return true; }
  const r = new FileReader();
  r.onload = () => {
    const url = String(r.result || "");
    const comma = url.indexOf(",");
    if (comma > 0) send({ type: "pasteImage", mime: file.type, name: file.name || "", data: url.slice(comma + 1) });
  };
  r.readAsDataURL(file);
  return true;
}

function insertImage(blob, alt, width) {
  if (!view || readOnly || !/^[0-9a-f]{64}$/.test(blob)) return;
  const node = S.image.create({ blob, alt: alt || "", width: width > 0 && width < 1 ? width : 1 });
  const { $from } = view.state.selection;
  const pos = $from.depth ? $from.after(1) : $from.pos;
  const tr = view.state.tr.insert(pos, node);
  tr.setSelection(NodeSelection.create(tr.doc, pos));   // selected, so its details show straight away
  view.dispatch(tr.scrollIntoView());
  view.focus();
}

// --------------------------------------------------------------- messages

// The Format menu.
const COMMANDS = {
  bold: () => toggleMark(M.strong), italic: () => toggleMark(M.em), underline: () => toggleMark(M.underline),
  smallcaps: () => toggleMark(M.smallcaps), link: () => toggleLink, draftNote: () => toggleMark(M.draft),
  body: () => setStyle("body"), h1: () => setBlockType(S.heading, { level: 1 }), h2: () => setBlockType(S.heading, { level: 2 }),
  h3: () => setBlockType(S.heading, { level: 3 }), quote: () => wrapIn(S.blockquote),
  bullets: () => wrapInList(S.bullet_list), numbers: () => wrapInList(S.ordered_list),
  comment: () => addComment, sceneBreak: () => insertSceneBreak, lineBreak: () => hardBreak, newScene: () => newSceneAtEnd,
};

function receive(msg) {
  if (!msg || typeof msg !== "object") return;
  if (msg.type === "load") load(msg);
  else if (msg.type === "flush") { flush(); send({ type: "flushed" }); }
  else if (msg.type === "title") { document.getElementById("title").textContent = msg.title || ""; setChapName(msg.title); }
  else if (msg.type === "theme") setTheme(!!msg.dark);
  else if (msg.type === "head") setHead(msg.head);
  else if (msg.type === "image") insertImage(msg.blob, msg.alt, msg.width);
  else if (msg.type === "setPicture") setPicture(msg.pos, msg.attrs || {});
  else if (msg.type === "removePicture") removePicture(msg.pos);
  else if (msg.type === "focus" && view) view.focus();
  else if (msg.type === "comments") {
    resolvedComments = new Set(Array.isArray(msg.resolved) ? msg.resolved : []);
    if (view) { view.dispatch(view.state.tr.setMeta("comments", true)); reportedComment = null; reportComment(view.state); }
  }
  else if (msg.type === "selectComment") selectComment(msg.id);
  else if (msg.type === "find") findWords(msg.words);
  else if (msg.type === "styleImported") styleImported(msg.data);
  else if (msg.type === "notice") notice(String(msg.text || ""));
  else if (msg.type === "view") {
    if ("comic" in msg) {
      comicMode = !!msg.comic;
      comicPageStart = Math.max(0, msg.comicPageStart | 0);
      const g = document.getElementById("comicstyles");
      if (g) g.hidden = !comicMode;
      document.documentElement.classList.toggle("comic", comicMode);
    }
    if (msg.settings && typeof msg.settings === "object") {
      viewSettings = { ...VIEW_DEFAULTS, ...msg.settings };
      // Settings from before 0.22.1 could have left hints off or quiet by default: show them, unless Off was chosen.
      if (!(msg.settings.v >= 2)) { if (msg.settings.hints !== "off") viewSettings.hints = "full"; viewSettings.v = 2; }
      // 0.49: fewer underlines. Everyone starts on Quiet (the essential hints) once; Full is a click away in Aa.
      if (!(msg.settings.v >= 3)) { if (viewSettings.hints === "full") viewSettings.hints = "quiet"; viewSettings.v = 3; setTimeout(() => send({ type: "view", settings: viewSettings }), 0); }
    }
    if ("names" in msg) { setNames(msg.names); setEntries(msg.names); }
    if ("bookFont" in msg) bookFontName = String(msg.bookFont || "");
    if ("fonts" in msg) setWorldFonts(msg.fonts);
    if (msg.style && typeof msg.style === "object") {
      if (msg.style.scope !== style.scope) { ignoreOnceSet = new Set(); pausedRules = new Set(); }
      style = { scope: String(msg.style.scope || ""), name: String(msg.style.name || "this book"), label: String(msg.style.label || "this book"),
        rules: Array.isArray(msg.style.rules) ? msg.style.rules : [], ignore: msg.style.ignore && typeof msg.style.ignore === "object" ? msg.style.ignore : {},
        words: Array.isArray(msg.style.words) ? msg.style.words.map(String) : [],
        languages: Array.isArray(msg.style.languages) ? msg.style.languages : [] };
      spell.setConlangs(style.languages);
      renderRules();
    }
    spell.setExtraWords([...nameWords, ...style.words]);
    applyView();
    stats();
  }
  else if (msg.type === "stripComment") stripComment(msg.id);
  else if (msg.type === "saved") { if (msg.id === currentId && !dirty) setSaved(msg.ok === false ? "failed" : "saved", msg.ok !== false); }
  else if (msg.type === "command") {
    if (msg.name === "picture") send({ type: "insertImage" });
    else if (COMMANDS[msg.name] && view && !readOnly) run(COMMANDS[msg.name]());
  }
}

if (host) host.addEventListener("message", (e) => receive(e.data));
window.studioReceive = receive;   // for testing in a plain browser
window.addEventListener("beforeunload", () => { flush(); rememberScroll(); if (view && currentId) rememberVersion(currentId, view.state.doc.toJSON(), true); });
document.addEventListener("visibilitychange", () => { if (document.hidden) flush(); });

buildToolbar();
buildViewMenu();
buildFontMenu();
buildChapterMenu();
buildSprintMenu();
buildMoreMenu();
fitBar();
initNameCard(send);
const styleApi = {
  view: () => viewSettings,
  changeView,
  counts: () => lastHints.counts,
  hintAt: (i) => lastHints.hints[i],
  blocks: () => (view ? textBlocks(view.state.doc) : []),
  rules: () => style.rules,
  ignore: () => style.ignore,
  scope: () => ({ name: style.name, label: style.label }),
  send,
  notice,
  focus: () => view && view.focus(),
  rehint: rehintNow,
  next: nextHint,
  saveRules: saveStyle,
  setCategory: (id, on) => changeView({ hintCats: { ...(viewSettings.hintCats || {}), [id]: on } }),
  ignoreOnce: (h) => { ignoreOnceSet.add(h.key + "|" + h.text.toLocaleLowerCase().replace(/\s+/g, " ") + "|" + h.context); rehintNow(); },
  suggestions: (word) => spell.suggestions(word),
  words: () => style.words,
  removeWord: (word) => {
    style.words = style.words.filter((w) => w !== word);
    spell.setExtraWords([...nameWords, ...style.words]);
    saveStyle();
    rehintNow();
  },
  replaceWord: (h, text) => {
    if (!view) return;
    const doc = view.state.doc;
    let from = Math.min(h.from, doc.content.size), to = Math.min(h.to, doc.content.size);
    if (doc.textBetween(from, to) !== h.text) {   // the text moved: find it again close by
      const around = doc.textBetween(Math.max(0, from - 200), Math.min(doc.content.size, to + 200), "\n", " ");
      const i = around.indexOf(h.text);
      if (i < 0) return;
      from = Math.max(0, from - 200) + i;
      to = from + h.text.length;
      if (doc.textBetween(from, to) !== h.text) return;
    }
    view.dispatch(text ? view.state.tr.insertText(text, from, to) : view.state.tr.delete(from, to));
    view.focus();
  },
  // A hint's fix: replaces the text it was worked out on, if that's still there.
  applyFix: (h, f) => styleApi.replaceWord({ from: f.from, to: f.to, text: f.orig }, f.text),
  synonyms: (text) => spell.synonyms(text),
  notice: (t) => notice(t),
  loadThesaurus: (cb) => spell.loadThesaurus(cb),
  // Selects a range and scrolls it into view (an echo's earlier use).
  reveal: (r) => {
    if (!view || !r) return;
    const max = view.state.doc.content.size;
    view.dispatch(view.state.tr.setSelection(TextSelection.create(view.state.doc, Math.min(r.from, max), Math.min(r.to, max))).scrollIntoView());
    view.focus();
  },
  // The selected words (up to six), where they are on screen, and a way to replace them.
  selection: () => {
    if (!view) return null;
    const { from, to, empty } = view.state.selection;
    let a = from, z = to;
    if (empty) {   // the word at the cursor
      const $p = view.state.doc.resolve(from), t = $p.parent.textContent, off = $p.parentOffset;
      let i = off, j = off;
      while (i > 0 && /[\p{L}'’-]/u.test(t[i - 1])) i--;
      while (j < t.length && /[\p{L}'’-]/u.test(t[j])) j++;
      if (i === j) return null;
      a = from - (off - i); z = from + (j - off);
    }
    // Leave out spaces and punctuation at either end ("loud." -> "loud").
    let raw = view.state.doc.textBetween(a, z, " ", " ");
    while (raw && !/[\p{L}\p{N}]/u.test(raw[0])) { raw = raw.slice(1); a++; }
    while (raw && !/[\p{L}\p{N}]/u.test(raw[raw.length - 1])) { raw = raw.slice(0, -1); z--; }
    const text = raw;
    if (!text || text.split(/\s+/).length > 6) return null;
    const c = view.coordsAtPos(a), e = view.coordsAtPos(z);
    return { from: a, to: z, text, rect: { left: c.left, top: c.top, bottom: Math.max(c.bottom, e.bottom), right: e.right } };
  },
  addWord: (word) => {
    style.words = [...new Set([...style.words, word])].sort((a, b) => a.localeCompare(b));
    spell.setExtraWords([...nameWords, ...style.words]);
    saveStyle();
    rehintNow();
  },
  ignoreAlways: (h) => {
    const low = h.text.toLocaleLowerCase().replace(/\s+/g, " ");
    style.ignore[h.key] = [...new Set([...(style.ignore[h.key] || []), low])];
    saveStyle();
    rehintNow();
  },
};
initStyleUi(styleApi);
applyView();
setTheme(window.matchMedia && window.matchMedia("(prefers-color-scheme: dark)").matches);
// A right-click on a word: the host is told the word (or the selected words) first, so the menu it
// opens can offer "Add “Maren” to the bible".
document.addEventListener("mousedown", (e) => {
  if (e.button !== 2 || !view || !view.dom.contains(e.target)) return;
  let text = "";
  const sel = view.state.selection;
  const hit = view.posAtCoords({ left: e.clientX, top: e.clientY });
  if (!sel.empty && hit && hit.pos >= sel.from && hit.pos <= sel.to) {
    text = view.state.doc.textBetween(sel.from, sel.to, " ", " ");
  } else if (hit) {
    const $p = view.state.doc.resolve(hit.pos), t = $p.parent.textContent, off = $p.parentOffset;
    let i = off, j = off;
    while (i > 0 && /[\p{L}'’-]/u.test(t[i - 1])) i--;
    while (j < t.length && /[\p{L}'’-]/u.test(t[j])) j++;
    // A name of several capitalised words ("Maren Holt"): the words either side that start with a capital too.
    const capital = (k) => /\p{Lu}/u.test(t[k] || "");
    if (i < j && capital(i)) {
      for (;;) {
        let a = i - 1;
        if (t[a] !== " ") break;
        let b = a;
        while (b > 0 && /[\p{L}'’-]/u.test(t[b - 1])) b--;
        if (b === a || !capital(b)) break;
        i = b;
      }
      for (;;) {
        if (t[j] !== " ") break;
        let b = j + 1, c = b;
        while (c < t.length && /[\p{L}'’-]/u.test(t[c])) c++;
        if (c === b || !capital(b)) break;
        j = c;
      }
    }
    text = t.slice(i, j);
  }
  text = text.replace(/^[^\p{L}]+|[^\p{L}]+$/gu, "").replace(/['’]s$/u, "").trim();
  send({ type: "contextWord", text: text.length <= 60 ? text : "" });
}, true);
send({ type: "ready" });

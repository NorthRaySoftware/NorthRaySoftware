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
//                         {type:"link", href} {type:"insertImage"} {type:"saved-state", dirty} {type:"navigate", delta}
//                         {type:"picture", id, pos, attrs} (a picture selected; pos null when none is)
//                         {type:"comment", id, node, quote} (a comment was made on the selection)
//                         {type:"commentFocus", id, node} (the cursor is in a comment; id null when it isn't)
// Comments from the app:  {type:"comments", resolved:[ids]} {type:"selectComment", id} {type:"stripComment", id}
// Pictures from the app:  {type:"image", blob, alt, width} {type:"setPicture", pos, attrs} {type:"removePicture", pos}

import { Schema } from "prosemirror-model";
import { EditorState, Plugin, TextSelection, NodeSelection } from "prosemirror-state";
import { EditorView, Decoration, DecorationSet } from "prosemirror-view";
import { baseKeymap, toggleMark, setBlockType, wrapIn, chainCommands, exitCode, lift } from "prosemirror-commands";
import { keymap } from "prosemirror-keymap";
import { history, undo, redo } from "prosemirror-history";
import { inputRules, InputRule, smartQuotes, ellipsis, emDash, wrappingInputRule, textblockTypeInputRule } from "prosemirror-inputrules";
import { splitListItem, liftListItem, sinkListItem, wrapInList } from "prosemirror-schema-list";
import { dropCursor } from "prosemirror-dropcursor";
import { gapCursor } from "prosemirror-gapcursor";

// ------------------------------------------------------------------ schema

export const STYLES = [
  ["body", "Body"], ["first", "First paragraph"], ["centered", "Centred"], ["epigraph", "Epigraph"],
  ["attribution", "Attribution"], ["verse", "Verse"], ["letter", "Letter"], ["note", "Note"],
];
const STYLE_IDS = STYLES.map((s) => s[0]);
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
    em: { parseDOM: [{ tag: "i" }, { tag: "em" }, { style: "font-style=italic" }], toDOM: () => ["em", 0] },
    strong: { parseDOM: [{ tag: "b" }, { tag: "strong" }, { style: "font-weight", getAttrs: (v) => /^(bold(er)?|[6-9]\d\d)$/.test(v) && null }], toDOM: () => ["strong", 0] },
    underline: { parseDOM: [{ tag: "u" }], toDOM: () => ["u", 0] },
    smallcaps: { parseDOM: [{ tag: "span.sc" }, { style: "font-variant=small-caps" }], toDOM: () => ["span", { class: "sc" }, 0] },
    strike: { parseDOM: [{ tag: "s" }, { tag: "del" }], toDOM: () => ["s", 0] },
    sup: { excludes: "sub", parseDOM: [{ tag: "sup" }], toDOM: () => ["sup", 0] },
    sub: { excludes: "sup", parseDOM: [{ tag: "sub" }], toDOM: () => ["sub", 0] },
    code: { parseDOM: [{ tag: "code" }], toDOM: () => ["code", 0] },
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

function selectComment(id) {
  if (!view) return;
  activeComment = id;
  const r = commentRange(view.state.doc, id);
  if (r) {
    const tr = view.state.tr.setSelection(TextSelection.create(view.state.doc, r.from, r.to)).scrollIntoView();
    tr.setMeta("comments", true);
    view.dispatch(tr);
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
  "Mod-b": toggleMark(M.strong), "Mod-i": toggleMark(M.em), "Mod-u": toggleMark(M.underline),
  "Mod-Shift-k": toggleMark(M.smallcaps), "Mod-k": toggleLink, "Mod-Alt-m": addComment,
  "Mod-Enter": insertSceneBreak, "Shift-Enter": hardBreak,
  "Mod-Alt-0": setStyle("body"), "Mod-Alt-1": setBlockType(S.heading, { level: 1 }),
  "Mod-Alt-2": setBlockType(S.heading, { level: 2 }), "Mod-Alt-3": setBlockType(S.heading, { level: 3 }),
  "Enter": splitListItem(S.list_item), "Tab": sinkListItem(S.list_item), "Shift-Tab": liftListItem(S.list_item),
  "Mod-[": lift,
};

const rules = inputRules({
  rules: [
    ...smartQuotes, ellipsis, emDash,
    wrappingInputRule(/^\s*>\s$/, S.blockquote),
    wrappingInputRule(/^\s*([-+*])\s$/, S.bullet_list),
    wrappingInputRule(/^(\d+)\.\s$/, S.ordered_list, (m) => ({ start: +m[1] }), (m, n) => n.childCount + n.attrs.start === +m[1]),
    textblockTypeInputRule(/^(#{1,3})\s$/, S.heading, (m) => ({ level: m[1].length })),
    // "***" or "* * *" on its own line: a scene break.
    new InputRule(/^(\*\s?\*\s?\*|#)$/, (state, match, start, end) => {
      const $s = state.doc.resolve(start);
      if ($s.parent.type !== S.paragraph || $s.depth !== 1) return null;
      const tr = state.tr.replaceWith($s.before(), $s.after(), [S.scene_break.create(), S.paragraph.create()]);
      return tr.setSelection(TextSelection.create(tr.doc, $s.before() + 2));
    }),
  ],
});

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
  send({ type: "change", id: currentId, doc: view.state.doc.toJSON(), words: docWords(view.state.doc) });
}

// "Saving..." while there are changes not yet stored, then "Saved".
function setSaved(state) {
  const el = document.getElementById("saved");
  if (!el) return;
  el.textContent = state === "saving" ? "Saving..." : state === "failed" ? "Not saved" : "Saved";
  el.dataset.state = state;
}

function stats() {
  if (!view) return;
  const { from, to } = view.state.selection;
  const sel = from === to ? 0 : countWords(view.state.doc.textBetween(from, to, " ", " "));
  const words = docWords(view.state.doc);
  document.getElementById("count").textContent =
    (sel ? sel.toLocaleString() + " of " : "") + words.toLocaleString() + (words === 1 ? " word" : " words");
  send({ type: "stats", id: currentId, words, selectionWords: sel });
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
      if (!v.state.doc.eq(prev.doc) || !v.state.selection.eq(prev.selection)) { stats(); updateToolbar(v.state); reportPicture(v.state); reportComment(v.state); }
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

function makeState(docJSON) {
  let doc;
  try { doc = schema.nodeFromJSON(docJSON); doc.check(); }
  catch (e) { doc = schema.node("doc", null, [schema.node("paragraph")]); }
  return EditorState.create({
    doc,
    plugins: [rules, keymap(keys), keymap(baseKeymap), history(), dropCursor({ color: "var(--accent)" }), gapCursor(), watcher, placeholder, commentLook],
  });
}

function load(msg) {
  flush();
  if (typeof msg.dark === "boolean") setTheme(msg.dark);
  currentId = msg.id;
  readOnly = !!msg.readOnly;
  placeholderText = msg.placeholder || "";
  document.documentElement.lang = msg.language || "en-GB";
  document.getElementById("title").textContent = msg.title || "";
  const state = makeState(msg.doc);
  if (!view) {
    view = new EditorView(document.getElementById("page"), {
      state,
      editable: () => !readOnly,
      attributes: { spellcheck: "true", "aria-label": "Manuscript text" },
      nodeViews: { image: (node) => new ImageView(node) },
      handleDOMEvents: {
        blur: () => { flush(); return false; },
        click: (v, e) => {
          const a = e.target.closest && e.target.closest("a[href]");
          if (a && (e.ctrlKey || e.metaKey)) { send({ type: "link", href: a.getAttribute("href") }); e.preventDefault(); return true; }
          return false;
        },
      },
      transformPastedHTML: (html) => html.replace(/<(script|style)[\s\S]*?<\/\1>/gi, ""),
    });
  } else {
    view.updateState(state);
  }
  dirty = false;
  setSaved("saved");
  stats();
  updateToolbar(view.state);
  // Jump (not scroll) to the top, so switching chapters doesn't flicker past the old one.
  const scroller = document.getElementById("scroller");
  scroller.style.scrollBehavior = "auto";
  scroller.scrollTop = 0;
  requestAnimationFrame(() => { scroller.style.scrollBehavior = ""; });
  if (!readOnly && msg.focus !== false) view.focus();
  reported = null;
  reportPicture(view.state);
  activeComment = null;
  reportedComment = null;
  reportComment(view.state);
}

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
}

function buildToolbar() {
  const sel = document.getElementById("style");
  for (const [id, label] of STYLES) sel.add(new Option(label, id));
  for (const l of [1, 2, 3]) sel.add(new Option("Heading " + l, "h" + l));
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
  bind("undo", undo);
  bind("redo", redo);
  bind("comment", addComment);
  for (const [id, delta] of [["prev", -1], ["next", 1]])
    document.getElementById(id).addEventListener("mousedown", (e) => { e.preventDefault(); navigate(delta)(); });
  document.getElementById("img").addEventListener("mousedown", (e) => { e.preventDefault(); send({ type: "insertImage" }); });
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
  smallcaps: () => toggleMark(M.smallcaps), link: () => toggleLink,
  body: () => setStyle("body"), h1: () => setBlockType(S.heading, { level: 1 }), h2: () => setBlockType(S.heading, { level: 2 }),
  h3: () => setBlockType(S.heading, { level: 3 }), quote: () => wrapIn(S.blockquote),
  bullets: () => wrapInList(S.bullet_list), numbers: () => wrapInList(S.ordered_list),
  comment: () => addComment, sceneBreak: () => insertSceneBreak, lineBreak: () => hardBreak, newScene: () => newSceneAtEnd,
};

function receive(msg) {
  if (!msg || typeof msg !== "object") return;
  if (msg.type === "load") load(msg);
  else if (msg.type === "flush") { flush(); send({ type: "flushed" }); }
  else if (msg.type === "title") document.getElementById("title").textContent = msg.title || "";
  else if (msg.type === "theme") setTheme(!!msg.dark);
  else if (msg.type === "image") insertImage(msg.blob, msg.alt, msg.width);
  else if (msg.type === "setPicture") setPicture(msg.pos, msg.attrs || {});
  else if (msg.type === "removePicture") removePicture(msg.pos);
  else if (msg.type === "focus" && view) view.focus();
  else if (msg.type === "comments") {
    resolvedComments = new Set(Array.isArray(msg.resolved) ? msg.resolved : []);
    if (view) { view.dispatch(view.state.tr.setMeta("comments", true)); reportedComment = null; reportComment(view.state); }
  }
  else if (msg.type === "selectComment") selectComment(msg.id);
  else if (msg.type === "stripComment") stripComment(msg.id);
  else if (msg.type === "saved") { if (msg.id === currentId && !dirty) setSaved(msg.ok === false ? "failed" : "saved"); }
  else if (msg.type === "command") {
    if (msg.name === "picture") send({ type: "insertImage" });
    else if (COMMANDS[msg.name] && view && !readOnly) run(COMMANDS[msg.name]());
  }
}

if (host) host.addEventListener("message", (e) => receive(e.data));
window.studioReceive = receive;   // for testing in a plain browser
window.addEventListener("beforeunload", flush);
document.addEventListener("visibilitychange", () => { if (document.hidden) flush(); });

buildToolbar();
setTheme(window.matchMedia && window.matchMedia("(prefers-color-scheme: dark)").matches);
send({ type: "ready" });

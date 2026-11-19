// What's pasted into the manuscript (0.37), made to look like it was typed here.
//
// - From Google Docs: its wrapper that made everything bold is taken off, and its bold and italic (kept as styles on
//   spans) are kept as bold and italic. From Word: its hidden markup, comments and styles go.
// - Straight quotes become curly ones, "--" a dash and "..." an ellipsis, as when typing (not in code).
// - Blank paragraphs used as gaps between paragraphs go; indents typed with tabs or spaces go; a line that is only
//   "***", "* * *", "#" or "~" becomes a scene break.
// - Plain text: blank lines divide paragraphs, and lines wrapped short inside a paragraph (an email, a PDF) are
//   joined; text with no blank lines keeps a paragraph per line.
import { Fragment, Slice } from "prosemirror-model";

export function cleanPastedHTML(html) {
  return stylesToTags(html

    .replace(/<(script|style|title|xml)[\s\S]*?<\/\1>/gi, "")
    .replace(/<!--\[if[\s\S]*?<!\[endif\]-->/gi, "")
    .replace(/<!--[\s\S]*?-->/g, "")
    .replace(/<\/?(meta|link|o:p|w:[a-z]+|v:[a-z]+)\b[^>]*>/gi, "")
    // Google Docs wraps the whole paste in <b style="font-weight:normal" id="docs-internal-guid-…">.
    .replace(/<b\b([^>]*\bid="docs-internal-guid[^"]*"[^>]*)>/gi, "<span$1>")
    .replace(/<b\b([^>]*font-weight:\s*(normal|400)[^>]*)>/gi, "<span$1>"));
}

// Bold, italic and so on kept as styles (Google Docs, many web pages) become tags. The editor's page doesn't apply
// inline styles (its security policy), so the styles are read from the attribute's text here.
function stylesToTags(html) {
  if (typeof DOMParser === "undefined" || !/style\s*=/i.test(html)) return html;
  let doc;
  try { doc = new DOMParser().parseFromString(html, "text/html"); } catch (e) { return html; }
  const els = Array.from(doc.body.querySelectorAll("[style]"));
  for (const el of els) {
    const st = {};
    for (const part of (el.getAttribute("style") || "").split(";")) {
      const i = part.indexOf(":");
      if (i > 0) st[part.slice(0, i).trim().toLowerCase()] = part.slice(i + 1).trim().toLowerCase();
    }
    el.removeAttribute("style");
    const tag = el.tagName.toLowerCase();
    const weight = st["font-weight"] || "";
    const deco = (st["text-decoration"] || "") + " " + (st["text-decoration-line"] || "");
    const wraps = [];
    if (/^(bold|bolder|[6-9]00)$/.test(weight) && tag !== "b" && tag !== "strong" && !/^h\d$/.test(tag)) wraps.push("strong");
    if (st["font-style"] === "italic" && tag !== "i" && tag !== "em") wraps.push("em");
    if (/underline/.test(deco) && tag !== "u" && tag !== "a") wraps.push("u");
    if (/line-through/.test(deco) && tag !== "s" && tag !== "del") wraps.push("s");
    if (st["vertical-align"] === "super" && tag !== "sup") wraps.push("sup");
    if (st["vertical-align"] === "sub" && tag !== "sub") wraps.push("sub");
    if (st["font-variant"] === "small-caps" || st["font-variant-caps"] === "small-caps") wraps.push("span.sc");
    for (const w of wraps) {
      const [name, cls] = w.split(".");
      const n = doc.createElement(name);
      if (cls) n.className = cls;
      while (el.firstChild) n.appendChild(el.firstChild);
      el.appendChild(n);
    }
  }
  return doc.body.innerHTML;
}

const SCENE_BREAK = /^\s*(\*\s*\*\s*\*[\s*]*|#|~|⁂|∴|-\s*-\s*-[\s-]*|_\s*_\s*_[\s_]*)\s*$/;

// Curly quotes, dashes and ellipses in a run of text, given the character before it.
export function typographic(text, before) {
  let out = "";
  let prev = before || "";
  for (let i = 0; i < text.length; i++) {
    const c = text[i];
    const opens = prev === "" || /[\s([{—–/-]/.test(prev);
    if (c === '"') out += opens ? "“" : "”";
    else if (c === "'") out += opens ? "‘" : "’";
    else if (c === "-" && text[i + 1] === "-" && text[i + 2] !== "-") { out += "—"; i++; }
    else if (c === "." && text[i + 1] === "." && text[i + 2] === "." && text[i + 3] !== ".") { out += "…"; i += 2; }
    else out += c;
    prev = out[out.length - 1];
  }
  return out;
}

function tidyInline(fragment, schema, open, beforeText) {
  const nodes = [];
  let before = beforeText || "";
  let lead = !open;   // still at the start: indents are taken off (not when joining text already there)
  fragment.forEach((n) => {
    if (n.isText) {
      let t = n.text;
      if (lead) { t = t.replace(/^[ \t 　]+/, ""); if (!t) return; }
      lead = false;
      const code = n.marks.some((m) => m.type.name === "code");
      t = t.replace(/[​‌‍﻿­]/g, "").replace(/[  ]/g, " ");
      if (!code) t = typographic(t, before);
      if (!t) return;
      before = t[t.length - 1];
      nodes.push(schema.text(t, n.marks));
    } else {
      if (n.type.name !== "hard_break") lead = false;
      else if (lead) return;   // a line break before any text
      before = n.type.name === "hard_break" ? "" : "￼";
      nodes.push(n);
    }
  });
  return Fragment.from(nodes);
}

function tidyBlocks(fragment, schema, top, openStart, before) {
  const out = [];
  const count = fragment.childCount;
  fragment.forEach((n, _, i) => {
    const open = top && i === 0 && openStart > 0;
    if (n.type.name === "paragraph") {
      if (SCENE_BREAK.test(n.textContent) && schema.nodes.scene_break && top && !open) { out.push(schema.nodes.scene_break.create()); return; }
      const inline = tidyInline(n.content, schema, open, open ? before : "");
      if (inline.size === 0 && count > 1) return;   // a blank paragraph used as a gap
      out.push(n.type.create(n.attrs, inline, n.marks));
    } else if (n.isTextblock) {
      out.push(n.type.create(n.attrs, tidyInline(n.content, schema, open, open ? before : ""), n.marks));
    } else if (n.isLeaf) {
      out.push(n);
    } else {
      out.push(n.type.create(n.attrs, tidyBlocks(n.content, schema, false, 0, ""), n.marks));
    }
  });
  // Nothing left of a container's content: keep one empty paragraph rather than an invalid node.
  if (!out.length && count) out.push(schema.nodes.paragraph.create());
  return Fragment.from(out);
}

// `before`: the character just before where it goes (for which way the first quote turns).
export function tidySlice(slice, schema, before) {
  try {
    const content = tidyBlocks(slice.content, schema, true, slice.openStart, before || "");
    if (!content.size) return slice;
    // Pasted inside one paragraph (a few words): keep the open sides so it joins the text around it.
    return new Slice(content, Math.min(slice.openStart, content.firstChild && content.firstChild.isTextblock ? slice.openStart : 0),
      Math.min(slice.openEnd, content.lastChild && content.lastChild.isTextblock ? slice.openEnd : 0));
  } catch (e) {
    return slice;
  }
}

// Plain text into paragraphs.
export function textParagraphs(text) {
  const t = text.replace(/\r\n?/g, "\n").replace(/[\u2028\u2029]/g, "\n");
  if (!/\n[ \t]*\n/.test(t)) return t.split("\n");
  // Blank lines divide paragraphs; within one, short-wrapped lines join (a list's lines stay apart).
  return t.split(/\n[ \t]*\n+/).map((block) => {
    const lines = block.split("\n");
    if (lines.some((l) => /^\s*([-*•]|\d+[.)])\s/.test(l))) return lines;
    return [lines.map((l) => l.trim()).join(" ")];
  }).flat();
}

export function textToSlice(text, schema) {
  const paras = textParagraphs(text);
  const nodes = paras.map((p) => schema.nodes.paragraph.create(null, p ? schema.text(p) : null));
  return new Slice(Fragment.from(nodes), nodes.length > 1 ? 1 : 1, 1);
}

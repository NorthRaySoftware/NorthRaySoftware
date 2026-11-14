// NorthRay Writing Studio: the style hints' interface in the editor.
//
// - The hint card: hovering an underline explains it, with Ignore once,
//   Ignore in this world (or book), and Turn off this kind of hint.
// - The hints list (click the hint count in the toolbar): Off / Quiet / Full,
//   each kind of hint on or off with its count in this chapter, the writer's
//   own rules, a reading-ease figure, and stepping from one hint to the next.
// - The rules editor: the writer's own rules for the world (or the book when
//   it isn't in a world), with a live count in the open chapter, import and export.
//
// Nothing here edits the text; the editor (editor.js) passes in what it needs.

import { CATEGORIES, CATEGORY, RULE_TYPES, analyse, readingEase, ruleProblem, newRuleId, readRuleSet } from "./hints.js";

let api = null;   // {state, send, rehint, view (settings), changeView, blocks, select, focus, scope}
const $ = (id) => document.getElementById(id);
const el = (tag, attrs = {}, ...kids) => {
  const e = document.createElement(tag);
  for (const [k, v] of Object.entries(attrs)) {
    if (k === "class") e.className = v;
    else if (k === "text") e.textContent = v;
    else if (k.startsWith("on")) e.addEventListener(k.slice(2), v);
    else if (v !== false && v != null) e.setAttribute(k, v === true ? "" : v);
  }
  for (const k of kids) if (k != null) e.append(k);
  return e;
};

// ------------------------------------------------------------------ card

let cardTimer = null, cardFor = null, hideTimer = null;

function showCard(target) {
  const hint = api.hintAt(+target.dataset.hint);
  if (!hint) return;
  const card = $("hintcard");
  card.replaceChildren();
  const scope = api.scope();
  if (hint.cat === "spell") {
    // A spelling mistake: suggestions to click, or keep the word.
    card.append(el("div", { class: "hc-name", text: "Spelling" }), el("div", { class: "hc-msg", text: hint.msg }));
    const sugg = api.suggestions(hint.text);
    const row = el("div", { class: "hc-sugg" });
    if (sugg.length) for (const s of sugg) row.append(el("button", { class: "hc-word", text: s, onclick: () => { api.replaceWord(hint, s); hideCard(true); } }));
    else row.append(el("span", { class: "hc-why", text: "No suggestions." }));
    card.append(row);
    const acts = el("div", { class: "hc-actions" });
    acts.append(el("button", { text: `Add to the dictionary for ${scope.name}`, title: `"${hint.text}" won't be marked again in ${scope.label}`,
      onclick: () => { api.addWord(hint.text); hideCard(true); } }));
    acts.append(el("button", { text: "Ignore once", onclick: () => { api.ignoreOnce(hint); hideCard(true); } }));
    card.append(acts);
    placeCard(card, target);
    return;
  }
  card.append(el("div", { class: "hc-name", text: hint.name || "Your rule" }), el("div", { class: "hc-msg", text: hint.msg || "" }));
  if (hint.why) card.append(el("div", { class: "hc-why", text: hint.why }));
  const row = el("div", { class: "hc-actions" });
  row.append(el("button", { text: "Ignore once", onclick: () => { api.ignoreOnce(hint); hideCard(true); } }));
  const word = hint.text.length > 28 ? hint.text.slice(0, 26) + "…" : hint.text;
  row.append(el("button", { text: `Ignore "${word}" in ${scope.label}`, title: `Not marked again anywhere in ${scope.name}`,
    onclick: () => { api.ignoreAlways(hint); hideCard(true); } }));
  if (hint.cat) row.append(el("button", { text: "Turn off this kind", onclick: () => { api.setCategory(hint.cat, false); hideCard(true); } }));
  else row.append(el("button", { text: "Edit the rule", onclick: () => { hideCard(true); openRules(hint.rule); } }));
  card.append(row);
  placeCard(card, target);
}

function placeCard(card, target) {
  card.hidden = false;
  const r = target.getBoundingClientRect();
  const w = Math.min(380, window.innerWidth - 16);
  card.style.width = w + "px";
  card.style.left = Math.max(8, Math.min(r.left, window.innerWidth - w - 8)) + "px";
  const below = r.bottom + 8, h = card.offsetHeight;
  card.style.top = (below + h < window.innerHeight - 8 ? below : Math.max(8, r.top - h - 8)) + "px";
  cardFor = target;
}
function hideCard(now) {
  clearTimeout(cardTimer);
  clearTimeout(hideTimer);
  const go = () => { $("hintcard").hidden = true; cardFor = null; };
  if (now) go(); else hideTimer = setTimeout(go, 250);
}

function wireCard() {
  document.addEventListener("mouseover", (e) => {
    const t = e.target.closest && e.target.closest(".hint[data-hint]");
    const inCard = e.target.closest && e.target.closest("#hintcard");
    if (inCard) { clearTimeout(hideTimer); return; }
    if (t) {
      clearTimeout(hideTimer);
      if (t === cardFor) return;
      clearTimeout(cardTimer);
      cardTimer = setTimeout(() => showCard(t), 380);
    } else if (cardFor || !$("hintcard").hidden) hideCard(false);
    else clearTimeout(cardTimer);
  });
  document.addEventListener("keydown", () => hideCard(true), true);
  $("scroller").addEventListener("scroll", () => hideCard(true));
}

// ------------------------------------------------------------- the list

function modeOf(v) { return v.hints === true ? "full" : v.hints === false || v.hints == null ? "quiet" : v.hints; }

export function renderList() {
  const box = $("hintlist");
  if (!box || box.hidden) return;
  const v = api.view(), mode = modeOf(v), counts = api.counts(), cats = v.hintCats || {};
  box.replaceChildren();
  const modes = el("div", { class: "vm-row" });
  for (const [m, label] of [["off", "Off"], ["quiet", "Quiet"], ["full", "Full"]])
    modes.append(el("button", { text: label, "aria-pressed": String(mode === m), onclick: () => { api.changeView({ hints: m }); } }));
  box.append(el("div", { class: "vm-label", text: "Style hints" }), modes,
    el("div", { class: "hl-note", text: mode === "quiet" ? "Quiet shows the clear slips only (doubled words, mixed spellings, redundancies, adverbs on dialogue tags) and your own rules."
      : mode === "full" ? "Full shows every kind ticked below." : "Only your own rules are shown." }));
  const list = el("div", { class: "hl-list" });
  for (const c of CATEGORIES) {
    const on = c.id in cats ? !!cats[c.id] : c.on;
    const shown = mode === "full" || (mode === "quiet" && c.essential);
    const n = counts["cat:" + c.id] || 0;
    const row = el("div", { class: "hl-row" + (shown && on ? "" : " hl-off") });
    const box2 = el("input", { type: "checkbox", title: on ? "Turn off" : "Turn on" });
    box2.checked = on;
    box2.addEventListener("change", () => api.setCategory(c.id, box2.checked));
    const name = el("button", { class: "hl-name", text: c.name, title: n ? "Go to the next one" : c.why, onclick: () => n && api.next("cat:" + c.id) });
    row.append(box2, el("span", { class: "hl-swatch hint-" + c.id }), name, el("span", { class: "hl-count", text: shown && on ? String(n) : "" }));
    list.append(row);
  }
  box.append(list);
  // The writer's own rules.
  const scope = api.scope();
  const rules = api.rules();
  box.append(el("div", { class: "vm-rule" }), el("div", { class: "vm-label", text: `Your rules for ${scope.name}` }));
  if (!rules.length) box.append(el("div", { class: "hl-note", text: `None yet. Rules belong to ${scope.label}, so every book in it shares them.` }));
  const rl = el("div", { class: "hl-list" });
  for (const r of rules) {
    const n = counts["rule:" + r.id] || 0;
    const row = el("div", { class: "hl-row" + (r.enabled === false ? " hl-off" : "") });
    row.append(el("span", { class: "hl-swatch hint-rule" }),
      el("button", { class: "hl-name", text: r.name || RULE_TYPES[r.type], onclick: () => (n ? api.next("rule:" + r.id) : openRules(r.id)) }),
      el("span", { class: "hl-count", text: r.enabled === false ? "off" : String(n) }));
    rl.append(row);
  }
  box.append(rl, el("button", { class: "hl-open", text: rules.length ? "Edit your rules…" : "Make a rule…", onclick: () => openRules() }));
  const ease = readingEase(api.blocks());
  if (ease != null)
    box.append(el("div", { class: "vm-rule" }), el("div", { class: "hl-note",
      text: `Reading ease ${ease} (Flesch; higher is easier, 60–70 is plain English). It counts word and sentence lengths only, so treat it as a rough guide.` }));
}

function wireList() {
  const btn = $("hints"), box = $("hintlist");
  btn.addEventListener("mousedown", (e) => {
    e.preventDefault();
    box.hidden = !box.hidden;
    if (!box.hidden) {
      const r = btn.getBoundingClientRect();
      box.style.left = Math.max(8, Math.min(r.left - 150, window.innerWidth - 372)) + "px";
      box.style.top = (r.bottom + 6) + "px";
      renderList();
    }
  });
  document.addEventListener("mousedown", (e) => { if (!box.hidden && !box.contains(e.target) && e.target !== btn) box.hidden = true; });
  document.addEventListener("keydown", (e) => { if (e.key === "Escape") box.hidden = true; });
}

// ------------------------------------------------------------ rules editor

let editing = null;   // the rule's id
let saveTimer = null;

function saveSoon() {
  clearTimeout(saveTimer);
  saveTimer = setTimeout(() => api.saveRules(), 500);
  api.rehint();
}

export function openRules(id) {
  $("hintlist").hidden = true;
  const box = $("rules");
  box.hidden = false;
  editing = id || (api.rules()[0] && api.rules()[0].id) || null;
  renderRules();
}
function closeRules() { $("rules").hidden = true; api.saveRules(); api.focus(); }

function matchesOf(rule) {
  if (ruleProblem(rule)) return null;
  return analyse(api.blocks(), { mode: "off", rules: [{ ...rule, enabled: true }], ignore: api.ignore() }).hints.length;
}

export function renderRules() {
  const box = $("rules");
  if (!box || box.hidden) return;
  const scope = api.scope(), rules = api.rules();
  box.replaceChildren();
  const head = el("div", { class: "rl-head" },
    el("div", {}, el("div", { class: "vm-label", text: "Style rules" }), el("h2", { text: scope.name }),
      el("div", { class: "hl-note", text: `Your own hints, shared by every book in ${scope.label}. They never change the text.` })),
    el("div", { class: "rl-tools" },
      el("button", { text: "Import…", title: "Add rules from a .nrstyle.json file", onclick: () => api.send({ type: "styleImport" }) }),
      el("button", { text: "Export…", title: "Save these rules as a file to share or keep", onclick: () => api.send({ type: "styleExport", data: exportData() }) }),
      el("button", { class: "primary", text: "Done", onclick: closeRules })));
  box.append(head);
  const body = el("div", { class: "rl-body" });
  // The list.
  const side = el("div", { class: "rl-side" });
  const add = el("select", { "aria-label": "New rule" });
  add.append(new Option("+ New rule…", ""));
  for (const [t, label] of Object.entries(RULE_TYPES)) add.append(new Option(label, t));
  add.addEventListener("change", () => {
    if (!add.value) return;
    const r = { id: newRuleId(), name: "New rule", type: add.value, terms: [], where: "all", wholeWord: true, enabled: true };
    if (r.type === "limit") { r.max = 3; r.per = "chapter"; }
    if (r.type === "nearby") r.within = 100;
    if (r.type === "replace") r.pairs = [];
    if (r.type === "pattern") r.pattern = "";
    api.rules().push(r);
    editing = r.id;
    saveSoon();
    renderRules();
    setTimeout(() => { const n = $("rf-name"); if (n) { n.focus(); n.select(); } }, 0);
  });
  side.append(add);
  for (const r of rules) {
    const n = matchesOf(r);
    side.append(el("button", { class: "rl-item" + (r.id === editing ? " sel" : "") + (r.enabled === false ? " off" : ""), onclick: () => { editing = r.id; renderRules(); } },
      el("span", { class: "rl-iname", text: r.name || RULE_TYPES[r.type] }),
      el("span", { class: "rl-ikind", text: RULE_TYPES[r.type] + (n == null ? "  ·  needs fixing" : `  ·  ${n} here`) })));
  }
  // The world's dictionary.
  const words = api.words ? api.words() : [];
  if (words.length) {
    side.append(el("div", { class: "vm-label rl-iglabel", text: "Dictionary" }));
    for (const wd of words)
      side.append(el("div", { class: "rl-ig" }, el("span", { text: wd }), el("span", { class: "rl-igk", text: "" }),
        el("button", { text: "×", title: "Take it out of the dictionary", onclick: () => { api.removeWord(wd); renderRules(); } })));
  }
  // Ignored words.
  const ign = Object.entries(api.ignore()).filter(([, v]) => v.length);
  if (ign.length) {
    side.append(el("div", { class: "vm-label rl-iglabel", text: "Ignored" }));
    for (const [k, words] of ign) {
      const label = k.startsWith("cat:") ? (CATEGORY[k.slice(4)] || {}).name : (rules.find((r) => "rule:" + r.id === k) || {}).name;
      for (const wd of words)
        side.append(el("div", { class: "rl-ig" }, el("span", { text: `"${wd}"`, title: label || "" }), el("span", { class: "rl-igk", text: label || "" }),
          el("button", { text: "×", title: "Mark it again", onclick: () => { api.ignore()[k] = words.filter((x) => x !== wd); saveSoon(); renderRules(); } })));
    }
  }
  body.append(side);
  // The form.
  const r = rules.find((x) => x.id === editing);
  body.append(r ? ruleForm(r) : el("div", { class: "rl-form rl-empty", text: rules.length ? "Choose a rule." :
    "Make your first rule with \"+ New rule\". For example: flag \"orbs\", suggest \"grey\" for \"gray\", keep \"Kathryn\" not \"Katherine\", or limit \"smiled\" to three a chapter." }));
  box.append(body);
}

function field(label, input, note) {
  return el("label", { class: "rf" }, el("span", { class: "rf-label", text: label }), input, note ? el("span", { class: "rf-note", text: note }) : null);
}

function ruleForm(r) {
  const form = el("div", { class: "rl-form" });
  const update = (patch) => { Object.assign(r, patch); saveSoon(); refreshCount(); };
  const count = el("div", { class: "rf-count" });
  const refreshCount = () => {
    const why = ruleProblem(r);
    const n = why ? null : matchesOf(r);
    count.textContent = why ? why : n === 0 ? "No matches in this chapter." : `${n} ${n === 1 ? "match" : "matches"} in this chapter.`;
    count.classList.toggle("bad", !!why);
    const item = document.querySelector(".rl-item.sel .rl-ikind");
    if (item) item.textContent = RULE_TYPES[r.type] + (n == null ? "  ·  needs fixing" : `  ·  ${n} here`);
    const nm = document.querySelector(".rl-item.sel .rl-iname");
    if (nm) nm.textContent = r.name || RULE_TYPES[r.type];
  };
  const text = (id, value, onInput, attrs = {}) => {
    const i = el("input", { id, type: "text", value: value || "", ...attrs });
    i.addEventListener("input", () => onInput(i.value));
    return i;
  };
  const area = (value, onInput, placeholder) => {
    const t = el("textarea", { rows: 5, placeholder: placeholder || "" });
    t.value = value || "";
    t.addEventListener("input", () => onInput(t.value));
    return t;
  };
  const lines = (s) => s.split(/\r?\n/).map((x) => x.trim()).filter(Boolean);

  form.append(el("div", { class: "vm-label", text: RULE_TYPES[r.type] }));
  form.append(field("Name", text("rf-name", r.name, (v) => update({ name: v }))));
  if (r.type === "flag" || r.type === "limit" || r.type === "nearby")
    form.append(field("Words or phrases", area((r.terms || []).join("\n"), (v) => update({ terms: lines(v) }), "One on each line"), "One on each line."));
  if (r.type === "consistent")
    form.append(field("Spellings", area((r.terms || []).join("\n"), (v) => update({ terms: lines(v) }), "Kathryn\nKatherine"),
      "One on each line, the one to keep first. The others are marked."));
  if (r.type === "replace")
    form.append(field("Replace", area((r.pairs || []).map((p) => p.from + " => " + p.to).join("\n"), (v) => update({
      pairs: lines(v).map((l) => { const [from, ...rest] = l.split(/\s*(?:=>|→)\s*/); return { from: from.trim(), to: rest.join(" ").trim() }; }),
    }), "gray => grey\ntoward => towards"), "One on each line: the word, then =>, then what you'd rather use."));
  if (r.type === "limit") {
    const max = el("input", { type: "number", min: "0", max: "999", value: String(r.max ?? 3) });
    max.addEventListener("input", () => update({ max: Math.max(0, +max.value || 0) }));
    const per = el("select", {});
    per.append(new Option("in a chapter", "chapter"), new Option("per 1,000 words", "1000"));
    per.value = r.per || "chapter";
    per.addEventListener("change", () => update({ per: per.value }));
    form.append(field("At most", el("span", { class: "rf-inline" }, max, per), "Uses after that are marked."));
  }
  if (r.type === "nearby") {
    const within = el("input", { type: "number", min: "2", max: "5000", value: String(r.within ?? 100) });
    within.addEventListener("input", () => update({ within: Math.max(2, +within.value || 2) }));
    form.append(field("Within", el("span", { class: "rf-inline" }, within, el("span", { text: " words of the last one" }))));
  }
  if (r.type === "pattern")
    form.append(field("Pattern", text("rf-pattern", r.pattern, (v) => update({ pattern: v }), { spellcheck: "false", class: "mono" }),
      "RE2 syntax, for example \\b(\\w+)ly (said|asked)\\b. Patterns can't freeze the editor: they run in linear time, without backreferences."));
  // Where and how.
  const where = el("select", {});
  where.append(new Option("Everywhere", "all"), new Option("Narration only", "narration"), new Option("Dialogue only", "dialogue"));
  where.value = r.where || "all";
  where.addEventListener("change", () => update({ where: where.value }));
  form.append(field("Where", where));
  const check = (key, label, def) => {
    const c = el("input", { type: "checkbox" });
    c.checked = key in r ? !!r[key] : def;
    c.addEventListener("change", () => update({ [key]: c.checked }));
    return el("label", { class: "vm-check rf-check" }, c, " " + label);
  };
  if (r.type !== "pattern") form.append(check("wholeWord", "Whole words only", true), check("inflections", "Include endings (-s, -ed, -ing, -ly)", false));
  form.append(check("caseSensitive", "Match capitals exactly", false), check("enabled", "On", true));
  form.append(field("Message", text("rf-msg", r.message, (v) => update({ message: v }), { placeholder: "Shown on the hint; %s is the matched words" })));
  form.append(field("Why", area(r.explanation, (v) => update({ explanation: v }), "Optional: the reason, for you or anyone you share the rules with")));
  form.append(count);
  form.append(el("div", { class: "rf-actions" },
    el("button", { class: "danger", text: "Delete this rule", onclick: () => {
      const list = api.rules();
      const i = list.indexOf(r);
      if (i >= 0) list.splice(i, 1);
      delete api.ignore()["rule:" + r.id];
      editing = list[0] ? list[0].id : null;
      saveSoon();
      renderRules();
    } })));
  refreshCount();
  return form;
}

function exportData() {
  const scope = api.scope();
  return { format: "nrstyle", version: 1, name: scope.name, exported: new Date().toISOString().slice(0, 10), rules: api.rules(), ignore: api.ignore() };
}

// A file the app read for us: add its rules (same id: replaced) and ignored words.
export function imported(data) {
  const { rules, ignore, problems } = readRuleSet(data);
  const mine = api.rules();
  let added = 0, replaced = 0;
  for (const r of rules) {
    const i = mine.findIndex((x) => x.id === r.id);
    if (i >= 0) { mine[i] = r; replaced++; } else { mine.push(r); added++; }
  }
  const ig = api.ignore();
  for (const [k, v] of Object.entries(ignore)) ig[k] = [...new Set([...(ig[k] || []), ...v])];
  api.saveRules();
  api.rehint();
  openRules(rules[0] && rules[0].id);
  const parts = [];
  if (added) parts.push(`${added} added`);
  if (replaced) parts.push(`${replaced} updated`);
  if (problems.length) parts.push(`${problems.length} skipped: ${problems.slice(0, 3).join("; ")}`);
  api.notice(parts.length ? "Rules imported: " + parts.join(", ") + "." : "That file had no rules.");
}

export function initStyleUi(a) {
  api = a;
  wireCard();
  wireList();
}

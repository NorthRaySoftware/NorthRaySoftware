// NorthRay Writing Studio: the spell checker.
//
// Hunspell dictionaries from SCOWL (British or American, by the book's language),
// read by nspell. Unlike the browser's own checker it knows the book: every name
// in the world's bible is a word, and each world (or independent book) has its own
// dictionary of words the writer adds. British -ize spellings are accepted wherever
// the -ise form is a word (Oxford spelling).

import nspell from "nspell";

let checker = null, lang = "", loading = null;
let stems = [];              // the dictionary's base words, for suggestions further off than one or two letters
const cache = new Map();
let extra = new Set();       // lower-cased: the bible's names and the world's dictionary
const COMMON = ["the", "and", "that", "have", "for", "not", "with", "you", "this", "but", "his", "from", "they", "say", "her", "she",
  "will", "one", "all", "would", "there", "their", "what", "out", "about", "who", "get", "which", "when", "make", "can", "like",
  "time", "just", "him", "know", "take", "into", "your", "some", "could", "them", "see", "other", "than", "then", "now", "look",
  "only", "come", "its", "over", "think", "also", "back", "after", "use", "two", "how", "our", "work", "first", "well", "way",
  "even", "new", "want", "because", "any", "these", "give", "day", "most", "was", "were", "had", "said", "been", "has", "are"];
const commonRank = new Map(COMMON.map((w, i) => [w, i]));

export function ready() { return !!checker; }

// Loads the dictionary for a language ("en-GB", "en-US", ...); calls back when it's ready.
export function load(language, onReady) {
  const want = /^en-US/i.test(language || "") ? "en-US" : "en-GB";
  if (want === lang && (checker || loading)) return;
  lang = want;
  checker = null;
  cache.clear();
  const base = "dict/" + want;
  loading = Promise.all([fetch(base + ".aff").then((r) => r.text()), fetch(base + ".dic").then((r) => r.text())])
    .then(([aff, dic]) => {
      if (lang !== want) return;
      checker = nspell(aff, dic);
      stems = dic.split(/\r?\n/).slice(1).map((l) => l.split("/")[0]).filter((w) => w.length > 1);
      loading = null;
      onReady && onReady(true);
    })
    .catch(() => { loading = null; onReady && onReady(false); });
}

export function setExtraWords(words) {
  extra = new Set((words || []).map((w) => String(w).toLocaleLowerCase()));
  cache.clear();
}

function known(word) {
  if (!checker) return true;
  const w = word.replace(/’/g, "'");
  const low = w.toLocaleLowerCase();
  if (extra.has(low) || extra.has(low.replace(/'s$/, ""))) return true;
  if (cache.has(w)) return cache.get(w);
  let ok = checker.correct(w) || checker.correct(low) || checker.correct(w.replace(/'s$/, ""));
  // Oxford spelling: organize, realization, if organise and realisation are words.
  if (!ok && lang === "en-GB" && /iz/i.test(w)) ok = checker.correct(w.replace(/iz/g, "is").replace(/IZ/g, "IS"));
  cache.set(w, ok);
  return ok;
}

const WORD = /[\p{L}\p{M}]+(?:['’][\p{L}\p{M}]+)*/gu;

// Misspelt words in a block of text: [{from, to, word}] (offsets in the text).
export function misspelt(text) {
  if (!checker) return [];
  const out = [];
  for (const m of text.matchAll(WORD)) {
    const w = m[0];
    if (w.length < 2) continue;
    const before = text[m.index - 1] || "", after = text[m.index + w.length] || "";
    if (/[\p{N}_@/\\.]/u.test(before) && before !== "." || /[\p{N}_@]/u.test(after)) continue;   // codes, addresses, numbers
    if (w.length > 1 && w === w.toLocaleUpperCase()) continue;                                   // CAPITALS (acronyms, shouting)
    if (!known(w)) out.push({ from: m.index, to: m.index + w.length, word: w });
  }
  return out;
}

// Damerau-Levenshtein distance, for ranking suggestions.
function distance(a, b) {
  const d = Array.from({ length: a.length + 1 }, (_, i) => [i]);
  for (let j = 1; j <= b.length; j++) d[0][j] = j;
  for (let i = 1; i <= a.length; i++)
    for (let j = 1; j <= b.length; j++) {
      const cost = a[i - 1] === b[j - 1] ? 0 : 1;
      d[i][j] = Math.min(d[i - 1][j] + 1, d[i][j - 1] + 1, d[i - 1][j - 1] + cost);
      if (i > 1 && j > 1 && a[i - 1] === b[j - 2] && a[i - 2] === b[j - 1]) d[i][j] = Math.min(d[i][j], d[i - 2][j - 2] + 1);
    }
  return d[a.length][b.length];
}

// Words further off than nspell looks ("skelingtons" -> "skeletons"): base words that start with the same
// letter, with the misspelling's own ending put on (if that makes a real word), within a few letters.
const ENDINGS = ["'s", "s", "es", "ed", "d", "ing", "er", "ers", "ly", "ness", "ment", "ments"];
function farSuggestions(low) {
  if (!checker || low.length < 4) return [];
  const limit = low.length <= 5 ? 2 : low.length <= 8 ? 3 : 4;
  const endings = ["", ...ENDINGS.filter((e) => low.endsWith(e) && low.length - e.length >= 3)];
  const found = new Map();
  for (const st of stems) {
    const s0 = st.toLocaleLowerCase();
    if (s0[0] !== low[0] || Math.abs(s0.length - low.length) > limit + 4) continue;
    for (const e of endings) {
      const forms = e ? [s0 + e] : [s0];
      if (e && s0.endsWith("e") && /^[ei]/.test(e)) forms.push(s0.slice(0, -1) + e);   // bake -> baking
      if (e && s0.endsWith("y") && e === "es") forms.push(s0.slice(0, -1) + "ies");
      for (const f of forms) {
        if (Math.abs(f.length - low.length) > limit || found.has(f)) continue;
        const d = distance(low, f);
        if (d <= limit && (e === "" || checker.correct(f))) found.set(f, d);
      }
    }
  }
  return [...found].sort((a, b) => a[1] - b[1]).slice(0, 8).map(([f]) => f);
}

// Up to `n` suggestions, closest (and most common) first, in the word's own capitalisation.
export function suggestions(word, n = 5) {
  if (!checker) return [];
  const w = word.replace(/’/g, "'");
  const low = w.toLocaleLowerCase();
  let list = checker.suggest(w);
  if (w !== low) list = list.concat(checker.suggest(low));
  if (list.length < 3) list = list.concat(farSuggestions(low));
  const seen = new Set();
  list = list.filter((s) => { const k = s.toLocaleLowerCase(); if (seen.has(k)) return false; seen.add(k); return true; });
  list.sort((a, b) => distance(low, a.toLocaleLowerCase()) - distance(low, b.toLocaleLowerCase()) ||
    (commonRank.get(a.toLocaleLowerCase()) ?? 999) - (commonRank.get(b.toLocaleLowerCase()) ?? 999) ||
    (a === a.toLocaleLowerCase() ? -1 : 1) + (b === b.toLocaleLowerCase() ? 1 : -1));
  const cap = w[0] === w[0].toLocaleUpperCase() && w.slice(1) === w.slice(1).toLocaleLowerCase();
  return list.slice(0, n).map((s) => (cap && s === s.toLocaleLowerCase() ? s[0].toLocaleUpperCase() + s.slice(1) : s))
    .map((s) => (word.includes("’") ? s.replace(/'/g, "’") : s));
}

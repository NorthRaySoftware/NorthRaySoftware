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
  // Words people often write as one.
  const SPLIT = { alot: "a lot", infact: "in fact", aswell: "as well", eachother: "each other", atleast: "at least", incase: "in case",
    noone: "no one", alright: "all right", inbetween: "in between", ofcourse: "of course", thankyou: "thank you", everytime: "every time" };
  if (SPLIT[low]) list.unshift(SPLIT[low]);
  // Always look further too: jumbled letters ("Capitan" -> "Captain") are often more than nspell's one or two edits.
  list = list.concat(farSuggestions(low));
  const seen = new Set();
  list = list.filter((s) => { const k = s.toLocaleLowerCase(); if (seen.has(k)) return false; seen.add(k); return true; });
  // Closest first; the same letters in another order count as nearly there.
  const sorted = (x) => [...x].sort().join("");
  const lowSorted = sorted(low);
  const score = (s) => { const k = s.toLocaleLowerCase(); return distance(low, k) - (sorted(k) === lowSorted ? 1.5 : 0); };
  list.sort((a, b) => score(a) - score(b) ||
    (commonRank.get(a.toLocaleLowerCase()) ?? 999) - (commonRank.get(b.toLocaleLowerCase()) ?? 999) ||
    (a === a.toLocaleLowerCase() ? -1 : 1) + (b === b.toLocaleLowerCase() ? 1 : -1));
  const cap = w[0] === w[0].toLocaleUpperCase() && w.slice(1) === w.slice(1).toLocaleLowerCase();
  return list.slice(0, n).map((s) => (cap && s === s.toLocaleLowerCase() ? s[0].toLocaleUpperCase() + s.slice(1) : s))
    .map((s) => (word.includes("’") ? s.replace(/'/g, "’") : s));
}

// Is this a word (the dictionary, the bible's names, the world's own words)?
export function isWord(word) { return !!checker && known(word); }

// ---------------------------------------------------------------- thesaurus
// WordNet 3.1 (Princeton's licence, see licenses/), boiled down by tools/build-thesaurus.mjs to
// {"word or phrase": "synonym|synonym|..."}: single words first, then phrases. Fetched on first use.
let thesaurus = null, thesaurusLoading = null;
export function loadThesaurus(onReady) {
  if (thesaurus) return onReady && onReady();
  if (!thesaurusLoading) thesaurusLoading = fetch("dict/thesaurus.json").then((r) => r.json()).then((d) => { thesaurus = d; }).catch(() => { thesaurus = {}; });
  thesaurusLoading.then(() => onReady && onReady());
}
export function thesaurusReady() { return !!thesaurus; }

// Endings a word can carry, how to take them off, and which kinds of word (n, v, a, r) they can go back on.
const FORMS = [
  { base: (w) => w, kinds: "nvar", put: null },
  { base: (w) => (/ies$/.test(w) ? w.slice(0, -3) + "y" : /(?:s|sh|ch|x|z)es$/.test(w) ? w.slice(0, -2) : /[^s]s$/.test(w) ? w.slice(0, -1) : null),
    kinds: "nv", put: (w) => (/(?:s|sh|ch|x|z|o)$/.test(w) ? w + "es" : /[^aeiou]y$/.test(w) ? w.slice(0, -1) + "ies" : w + "s") },
  { base: (w) => (/ied$/.test(w) ? w.slice(0, -3) + "y" : /ed$/.test(w) ? w.slice(0, -2) : null), kinds: "v", put: past },
  { base: (w) => (/ed$/.test(w) ? w.slice(0, -1) : null), kinds: "v", put: past },
  { base: (w) => (/([^aeiou])\1ed$/.test(w) ? w.slice(0, -3) : null), kinds: "v", put: past },
  { base: (w) => (/ing$/.test(w) ? w.slice(0, -3) : null), kinds: "v", put: ing },
  { base: (w) => (/ing$/.test(w) ? w.slice(0, -3) + "e" : null), kinds: "v", put: ing },
  { base: (w) => (/([^aeiou])\1ing$/.test(w) ? w.slice(0, -4) : null), kinds: "v", put: ing },
  { base: (w) => (/ily$/.test(w) ? w.slice(0, -3) + "y" : /ly$/.test(w) ? w.slice(0, -2) : null), kinds: "a",
    put: (w) => (/[^aeiou]y$/.test(w) ? w.slice(0, -1) + "ily" : /le$/.test(w) ? w.slice(0, -1) + "y" : /ic$/.test(w) ? w + "ally" : w + "ly") },
];
const IRREG = { go: ["went", "going"], run: ["ran", "running"], die: ["died", "dying"], see: ["saw", "seeing"], come: ["came", "coming"],
  make: ["made", "making"], take: ["took", "taking"], get: ["got", "getting"], give: ["gave", "giving"], fall: ["fell", "falling"],
  leave: ["left", "leaving"], buy: ["bought", "buying"], drop: ["dropped", "dropping"], pop: ["popped", "popping"], shut: ["shut", "shutting"],
  stop: ["stopped", "stopping"], step: ["stepped", "stepping"], hit: ["hit", "hitting"], sit: ["sat", "sitting"], stand: ["stood", "standing"],
  think: ["thought", "thinking"], tell: ["told", "telling"], say: ["said", "saying"], speak: ["spoke", "speaking"], find: ["found", "finding"],
  hold: ["held", "holding"], bring: ["brought", "bringing"], keep: ["kept", "keeping"], feel: ["felt", "feeling"], lie: ["lay", "lying"] };
function past(w) {
  if (IRREG[w]) return IRREG[w][0];
  if (/e$/.test(w)) return w + "d";
  if (/[^aeiou]y$/.test(w)) return w.slice(0, -1) + "ied";
  const d = w + w.slice(-1) + "ed";
  return /^[^aeiou]*[aeiou][bdgmnprt]$/.test(w) && isWord(d) ? d : w + "ed";
}
function ing(w) {
  if (IRREG[w]) return IRREG[w][1];
  if (/ie$/.test(w)) return w.slice(0, -2) + "ying";
  if (/[^e]e$/.test(w)) return w.slice(0, -1) + "ing";
  const d = w + w.slice(-1) + "ing";
  return /^[^aeiou]*[aeiou][bdgmnprt]$/.test(w) && isWord(d) ? d : w + "ing";
}
// Puts the ending back: a verb phrase on its first word ("kick the bucket" -> "kicked the bucket"); nouns and
// adjectives only when they're one word.
function inflect(phrase, kind, put) {
  const parts = phrase.split(" ");
  if (parts.length > 1 && kind !== "v") return null;
  const w = put(parts[0]);
  if (!w || !isWord(w)) return null;
  parts[0] = w;
  return parts.join(" ");
}

// Other words and phrases for a word or phrase, in its form and capitalisation.
export function synonyms(text, n = 12) {
  if (!thesaurus) return [];
  const orig = text.trim().replace(/’/g, "'").replace(/\s+/g, " ");
  const low = orig.toLocaleLowerCase().replace(/'s$/, "");
  const out = [];
  const parts = low.split(" ");
  for (const f of FORMS) {
    // A phrase's ending is on its first word if it's a verb ("passed away"), otherwise its last.
    for (const at of parts.length > 1 ? [0, parts.length - 1] : [0]) {
      const b = f.base(parts[at]);
      if (!b) continue;
      const key = [...parts.slice(0, at), b, ...parts.slice(at + 1)].join(" ");
      const entry = thesaurus[key];
      if (!entry) continue;
      for (const seg of entry.split(";")) {
        const kind = seg[0];
        if (!f.kinds.includes(kind)) continue;
        if (f.put && parts.length > 1 && (kind !== "v" || at !== 0)) continue;
        for (const s of seg.slice(2).split("|")) {
          const v = f.put ? inflect(s, kind, f.put) : s;
          if (v && v !== low && !out.includes(v)) out.push(v);
        }
      }
    }
  }
  // Words before phrases, keeping each one's order.
  const sorted = [...out.filter((x) => !x.includes(" ")).slice(0, Math.max(6, n - 4)), ...out.filter((x) => x.includes(" "))].slice(0, n);
  const upper = orig.length > 1 && orig === orig.toLocaleUpperCase() && orig !== orig.toLocaleLowerCase();
  const cap = !upper && orig[0] !== orig[0].toLocaleLowerCase();
  return sorted.map((s) => (upper ? s.toLocaleUpperCase() : cap ? s[0].toLocaleUpperCase() + s.slice(1) : s));
}

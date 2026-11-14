// Builds apps/studio/web/dict/thesaurus.json from WordNet 3.1 (npm i wordnet-db@3.1.14 next to this script first).
// {"word or phrase": "n:syn|syn;v:...;a:...;r:..."}: kinds by how often the word is used that way.
import fs from "fs";
const D = new URL("node_modules/wordnet-db/dict/", import.meta.url).pathname;
const web = "/home/claude/northray-source/apps/studio/web/dict/";
const dictWords = new Set();
for (const f of ["en-GB.dic", "en-US.dic"]) for (const l of fs.readFileSync(web + f, "utf8").split("\n").slice(1)) { const w = l.split("/")[0]; if (/^[a-z]+$/.test(w)) dictWords.add(w); }
const data = {};
for (const pos of ["noun", "verb", "adj", "adv"]) {
  const buf = fs.readFileSync(D + "data." + pos, "latin1"); data[pos] = buf;
}
function synset(pos, off) {
  const buf = data[pos]; const s = buf.indexOf("\n", +off); const line = buf.slice(+off, s);
  const p = line.split(" "); const n = parseInt(p[3], 16); const words = [];
  for (let i = 0; i < n; i++) words.push(p[4 + i * 2].replace(/\(.*\)$/, "").toLowerCase().replace(/_/g, " "));
  let k = 4 + n * 2; const pc = +p[k++]; const ptrs = [];
  for (let i = 0; i < pc; i++) { ptrs.push({ sym: p[k], off: p[k + 1], pos: p[k + 2] }); k += 4; }
  return { words, ptrs };
}
const okWord = (w) => { const p = w.split(" "); return p.length <= 4 && p.every((x) => /^[a-z]+$/.test(x) && dictWords.has(x)) && !(p.length > 1 && p.every((x) => x.length < 2)); };
const freq = new Map();   // synset offset -> how often its senses are used (tag counts)
for (const l of fs.readFileSync(D + "index.sense", "latin1").split("\n")) { const p = l.split(" "); if (p.length < 4) continue; freq.set(p[1], (freq.get(p[1]) || 0) + +p[3]); }
const POS = { n: "noun", v: "verb", a: "adj", s: "adj", r: "adv" };
const out = Object.create(null);
for (const pos of ["noun", "verb", "adj", "adv"]) {
  for (const line of fs.readFileSync(D + "index." + pos, "latin1").split("\n")) {
    if (!line || line[0] === " ") continue;
    const p = line.split(" "); const lemma = p[0];
    const lem = lemma.replace(/_/g, " ");
    if (!okWord(lem)) continue;
    const sc = +p[2], pc = +p[3]; const tag = +p[5 + pc]; const offs = p.slice(6 + pc, 6 + pc + sc);
    const list = [];
    offs.slice(0, 3).forEach((o, si) => {
      const ss = synset(pos, o);
      for (const w of ss.words) list.push(w);
      if ((pos === "noun" || pos === "verb") && si === 0 && ss.words.length < 3) for (const q of ss.ptrs.filter((q) => q.sym === "~").sort((x, y) => (freq.get(y.off) || 0) - (freq.get(x.off) || 0)).slice(0, 8)) list.push(...synset(pos, q.off).words.slice(0, 1));
      if (pos === "adj" && si < 2) for (const q of ss.ptrs.filter((q) => q.sym === "&").sort((x, y) => (freq.get(y.off) || 0) - (freq.get(x.off) || 0)).slice(0, 6)) for (const w of synset(POS[q.pos], q.off).words) list.push(w);
    });
    const syn = [...new Set(list)].filter((w) => w !== lem && okWord(w));
    if (!syn.length) continue;
    (out[lem] ||= []).push({ tag, syn, pos: pos[0] === "a" && pos === "adv" ? "r" : pos === "adv" ? "r" : pos[0] });
  }
}
const final = Object.create(null);
for (const [w, groups] of Object.entries(out)) {
  groups.sort((a, b) => b.tag - a.tag);
  let left = 12;
  const segs = [];
  for (const g of groups) {
    const words = g.syn.filter((x) => !x.includes(" ")).slice(0, 8), phrases = g.syn.filter((x) => x.includes(" ")).slice(0, 4);
    const take = [...words, ...phrases].slice(0, Math.max(4, left));
    left -= take.length;
    if (take.length) segs.push(g.pos + ":" + take.join("|"));
  }
  final[w] = segs.join(";");
}
fs.writeFileSync("/home/claude/northray-source/apps/studio/web/dict/thesaurus.json", JSON.stringify(final));
console.log(Object.keys(final).length, final.walk, "||", final.die, "||", final.loud);

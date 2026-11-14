// Tests for the style hints (node --test test/).
import { test } from "node:test";
import assert from "node:assert/strict";
import { analyse, dialogueRanges, sentences, ruleProblem, readRuleSet } from "../src/hints.js";

const run = (texts, opts = {}) => analyse(texts.map((text, i) => ({ text, pos: i * 1000 })), { mode: "full", ...opts });
const kinds = (r) => r.hints.map((h) => h.cat || h.rule);
const of = (r, k) => r.hints.filter((h) => (h.cat || h.rule) === k).map((h) => h.text);

test("dialogue: double and single quotes, apostrophes", () => {
  const t = "‘I don’t know,’ she said. The dog’s bowl was empty. ‘Come here.’";
  const r = dialogueRanges(t).map(([a, b]) => t.slice(a, b));
  assert.deepEqual(r, ["‘I don’t know,’", "‘Come here.’"]);
  const d = "“Wait,” he said, “for me.”";
  assert.deepEqual(dialogueRanges(d).map(([a, b]) => d.slice(a, b)), ["“Wait,”", "“for me.”"]);
  const open = "“This goes on into the next paragraph";
  assert.deepEqual(dialogueRanges(open), [[0, open.length]]);
  assert.deepEqual(dialogueRanges("The dogs’ bowls and ’cause."), []);
});

test("sentences keep titles together", () => {
  const t = "Mr. Holt came in. He sat down! Did he?";
  assert.equal(sentences(t).length, 3);
});

test("doubled words, but not had had", () => {
  const r = run(["She went to the the harbour. He had had enough."]);
  assert.deepEqual(of(r, "doubled"), ["the the"]);
});

test("quiet mode shows essential kinds only", () => {
  const r = run(["She was very, really tired and the the door opened."], { mode: "quiet" });
  assert.ok(kinds(r).every((k) => ["doubled", "spelling", "redundancy", "tagAdverb"].includes(k)), kinds(r).join());
  assert.equal(run(["the the"], { mode: "off" }).hints.length, 0);
});

test("adverbs on dialogue tags, not in narration elsewhere", () => {
  const r = run(["“Go,” she said softly.", "He walked slowly home.", "“Stay,” he quietly whispered."]);
  assert.deepEqual(of(r, "tagAdverb"), ["softly", "quietly"]);
  const early = run(["“Up,” she said early."]);
  assert.deepEqual(of(early, "tagAdverb"), []);
});

test("fancy dialogue tags only next to dialogue", () => {
  const r = run(["“Out,” she hissed.", "The kettle hissed on the stove."]);
  assert.deepEqual(of(r, "bookism"), ["hissed"]);
});

test("redundancy and cliché in narration, not in speech", () => {
  const r = run(["He nodded his head. Time stood still.", "“Time stood still,” she said."]);
  assert.deepEqual(of(r, "redundancy"), ["nodded his head"]);
  assert.deepEqual(of(r, "cliche"), ["Time stood still"]);
});

test("echoes skip common words and names", () => {
  const names = new Set(["maren"]);
  const r = run(["Maren saw the lantern. The lantern swung. Maren waited by the gate."], { names });
  assert.deepEqual(of(r, "echo"), ["lantern"]);
});

test("repeated sentence starts", () => {
  const r = run(["She ran. She fell. She rose. He watched."]);
  assert.deepEqual(of(r, "starts"), ["She"]);
});

test("mixed spellings and dashes mark the less-used form", () => {
  const r = run(["She realised it. He realised too. Then she realized again.", "Grey sky—grey sea – grey stone—"]);
  assert.deepEqual(of(r, "spelling"), ["realized", "–"]);
  // A tie: the book's language decides.
  assert.deepEqual(of(run(["She realised. He realized."], { lang: "en-GB" }), "spelling"), ["realized"]);
  assert.deepEqual(of(run(["She realised. He realized."], { lang: "en-US" }), "spelling"), ["realised"]);
});

test("filler words only when two or more in a paragraph", () => {
  assert.deepEqual(of(run(["It was very late."]), "filler"), []);
  assert.deepEqual(of(run(["It was very late and really cold."]), "filler"), ["very", "really"]);
});

test("possible passive leaves adjectives alone", () => {
  const r = run(["The door was opened by Tomas. She was tired. The letter was written."], { categories: { passive: true } });
  assert.deepEqual(of(r, "passive"), ["was opened by", "was written"]);
});

test("categories can be turned off", () => {
  const r = run(["the the"], { categories: { doubled: false } });
  assert.equal(r.hints.length, 0);
});

test("ignore lists and ignore once", () => {
  const r = run(["the the. the the."], { ignore: { "cat:doubled": ["the the"] } });
  assert.equal(r.hints.length, 0);
});

test("custom rules: flag, replace, limit, nearby, consistent, pattern", () => {
  const text = ["Her orbs glittered. The gray cat smiled. She smiled. He smiled. Katherine waved; Kathryn smiled.",
    "“My orbs,” she said."];
  const rules = [
    { id: "r1", name: "orbs", type: "flag", terms: ["orb"], inflections: true, where: "narration" },
    { id: "r2", name: "grey", type: "replace", pairs: [{ from: "gray", to: "grey" }] },
    { id: "r3", name: "smiled", type: "limit", terms: ["smiled"], max: 2, per: "chapter" },
    { id: "r4", name: "near", type: "nearby", terms: ["smiled"], within: 3 },
    { id: "r5", name: "Kathryn", type: "consistent", terms: ["Kathryn", "Katherine"] },
    { id: "r6", name: "pattern", type: "pattern", pattern: "\\bglitter\\w*" },
  ];
  const r = analyse(text.map((t, i) => ({ text: t, pos: i * 1000 })), { mode: "off", rules });
  const by = (id) => r.hints.filter((h) => h.rule === id).map((h) => h.text);
  assert.deepEqual(by("r1"), ["orbs"]);
  assert.deepEqual(by("r2"), ["gray"]);
  assert.match(r.hints.find((h) => h.rule === "r2").msg, /grey/);
  assert.deepEqual(by("r3"), ["smiled", "smiled"]);
  assert.deepEqual(by("r4"), ["smiled", "smiled"]);
  assert.deepEqual(by("r5"), ["Katherine"]);
  assert.deepEqual(by("r6"), ["glittered"]);
});

test("patterns that could run away are safe, and bad ones are refused", () => {
  assert.equal(ruleProblem({ type: "pattern", pattern: "(a+)+$" }), "");
  assert.notEqual(ruleProblem({ type: "pattern", pattern: "(unclosed" }), "");
  assert.notEqual(ruleProblem({ type: "pattern", pattern: "(\\w)\\1" }), "");   // backreferences aren't supported
  const t0 = Date.now();
  const r = analyse([{ text: "a".repeat(5000) + "b", pos: 0 }], { mode: "off", rules: [{ id: "x1", type: "pattern", pattern: "(a+)+$" }] });
  assert.ok(Date.now() - t0 < 1000);
  assert.equal(r.hints.length, 0);
});

test("rule files: kept fields only, broken rules skipped", () => {
  const { rules, problems, ignore } = readRuleSet({ rules: [
    { id: "abcd1234", name: "ok", type: "flag", terms: ["orbs"], evil: "x" },
    { name: "bad", type: "pattern", pattern: "(" },
    { name: "huh", type: "nope" },
  ], ignore: { "cat:echo": ["lantern"], "bad key": ["x"] } });
  assert.equal(rules.length, 1);
  assert.equal(rules[0].evil, undefined);
  assert.equal(problems.length, 2);
  assert.deepEqual(Object.keys(ignore), ["cat:echo"]);
  assert.equal(readRuleSet("nonsense").rules.length, 0);
});

test("a long chapter stays quick", () => {
  const para = "Maren walked slowly along the quay, and the wind came in off the sea, very cold and really sharp. “Go,” she said softly. ";
  const blocks = Array.from({ length: 400 }, (_, i) => ({ text: para.repeat(3), pos: i * 2000 }));
  const t0 = Date.now();
  analyse(blocks, { mode: "full", categories: { filter: true, passive: true, hedge: true, began: true, thereIs: true } });
  const ms = Date.now() - t0;
  assert.ok(ms < 1500, ms + " ms");
});

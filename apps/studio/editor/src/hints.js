// NorthRay Writing Studio: style hints.
//
// Suggestions only: nothing here changes the text. Given a chapter's text
// blocks, returns ranges to underline, each with its category, a short
// message and the reason behind it. It knows dialogue from narration, so
// most word-choice hints leave characters' speech alone, and it skips the
// names from the world's bible.
//
// The writer's own rules (per world, or per book outside any world) come in
// the same pass: flag words, suggest a replacement, limit how often a word
// is used, flag a word used again too soon, keep one spelling, or (for those
// who want it) a pattern, run with RE2 so no pattern can freeze the editor.
//
// Word lists here were written for NorthRay; no third-party list is copied.

import { RE2JS } from "re2js";

// ------------------------------------------------------------- categories

// essential: shown in Quiet mode too. on: the default when the writer hasn't chosen.
export const CATEGORIES = [
  { id: "doubled", name: "Doubled words", essential: true, on: true,
    why: "The same word twice in a row is usually a slip. (\"had had\" and \"that that\" are left alone.)" },
  { id: "spelling", name: "Mixed spellings and dashes", essential: true, on: true,
    why: "Both forms are fine, but a book usually keeps to one: -ise or -ize, grey or gray, closed em dashes or spaced en dashes. The less-used form is marked." },
  { id: "redundancy", name: "Redundancies", essential: true, on: true,
    why: "Words that repeat what's already said (\"nodded his head\": nodding is done with the head). Narration only." },
  { id: "tagAdverb", name: "Adverbs in dialogue tags", essential: true, on: true,
    why: "An adverb on \"said\" (said softly) often tells what the dialogue should show. Most editors suggest letting the words or an action carry it." },
  { id: "bookism", name: "Fancy dialogue tags", on: true,
    why: "Tags other than said and asked draw attention to themselves. Marked only when it's overdone: tags that are actions (she smiled), more fancy tags than plain ones, or the same one again and again." },
  { id: "echo", name: "Echoes", on: true,
    why: "The same distinctive word again within a few lines. Readers notice the repeat. Names from the bible and common words are left alone." },
  { id: "starts", name: "Repeated sentence starts", on: true,
    why: "Three or more sentences in a row beginning with the same word can make the rhythm monotonous. Narration only." },
  { id: "cliche", name: "Clichés", on: true,
    why: "A phrase readers have met many times. It can be fine in a character's voice, so dialogue is left alone." },
  { id: "filler", name: "Filler words", on: true,
    why: "Words that often add little (very, really, just). Marked only where a paragraph of narration has two or more." },
  { id: "long", name: "Long sentences", on: true,
    why: "A sentence over 40 words. Long sentences can be deliberate rhythm; this is only a prompt to check it reads well. Dialogue is left alone." },
  { id: "began", name: "Started to, began to", on: false,
    why: "\"She began to run\" can often be \"she ran\", unless the starting itself matters." },
  { id: "thereIs", name: "There was, there were", on: false,
    why: "Sentences opening with \"There was\" can be flat; the real subject may make a stronger start." },
  { id: "filter", name: "Filter words (show, don't tell)", on: false,
    why: "Saw, heard, felt, noticed: they put the reader behind the character instead of in the scene (Janet Burroway's \"filtering\"). Keep them when the noticing matters, or in a deliberately distant voice." },
  { id: "passive", name: "Possible passive", on: false,
    why: "A form of \"be\" with a past participle. Passives are not errors (linguists such as Geoffrey Pullum show the rule against them is a myth); they suit an unknown or unimportant actor. Some marked words are adjectives (she was tired)." },
  { id: "hedge", name: "Hedges", on: false,
    why: "Seemed to, sort of, a little: they soften a statement. Fine in a hesitant voice; in narration they can blur the picture." },
];
export const CATEGORY = Object.fromEntries(CATEGORIES.map((c) => [c.id, c]));

// ------------------------------------------------------------------ words

const L = "\\p{L}\\p{N}";
const W = (body) => `(?<![${L}'’])(?:${body})(?![${L}])`;   // whole words, Unicode-aware
const WORD = /[\p{L}\p{N}]+(?:['’\-‐‑][\p{L}\p{N}]+)*/gu;
export function countWords(text) { const m = text.match(WORD); return m ? m.length : 0; }

const SPEECH = ["said", "says", "say", "asked", "asks", "ask", "replied", "replies", "answered", "answers", "told", "tells", "added", "adds"];
const BOOKISMS = ["whispered", "murmured", "muttered", "hissed", "growled", "snarled", "snapped", "barked", "bellowed", "roared",
  "thundered", "shrieked", "screamed", "yelled", "shouted", "cried", "exclaimed", "declared", "announced", "proclaimed",
  "retorted", "quipped", "chuckled", "laughed", "giggled", "sighed", "groaned", "moaned", "whimpered", "sobbed", "breathed",
  "purred", "spat", "gasped", "stammered", "stuttered", "blurted", "interjected", "opined", "intoned", "chided", "scolded",
  "admonished", "implored", "pleaded", "begged", "demanded", "insisted", "queried", "enquired", "inquired", "wondered",
  "mused", "smirked", "grinned", "smiled", "beamed", "huffed", "sniffed", "scoffed", "drawled", "rasped", "croaked", "squeaked"];
// Tags that are actions, not ways of speaking: always worth a look ("she smiled" can't carry words).
const ACTION_TAGS = new Map([["smiled", "smile"], ["grinned", "grin"], ["smirked", "smirk"], ["beamed", "beam"], ["laughed", "laugh"],
  ["chuckled", "chuckle"], ["giggled", "giggle"], ["sighed", "sigh"], ["sniffed", "sniff"], ["huffed", "huff"], ["scoffed", "scoff"]]);
const ordinal = (n) => (n === 3 ? "third" : n === 4 ? "fourth" : n === 5 ? "fifth" : n + "th");
// Words ending in -ly that aren't manner adverbs.
const NOT_ADVERBS = new Set(["early", "only", "family", "holy", "ugly", "lonely", "friendly", "silly", "lovely", "likely", "daily",
  "weekly", "monthly", "yearly", "fly", "reply", "supply", "apply", "rely", "ally", "belly", "jelly", "bully", "lily", "rally",
  "sally", "tally", "italy", "july", "lily", "curly", "surly", "burly", "jolly", "folly", "hilly", "chilly", "costly", "deadly",
  "elderly", "kindly", "lively", "orderly", "timely", "unlikely", "ugly", "wily", "comely", "homely", "manly", "womanly",
  "brotherly", "sisterly", "motherly", "fatherly", "scholarly", "beastly", "ghostly", "ghastly", "heavenly", "worldly", "cowardly"]);
const FILLERS = ["very", "really", "just", "actually", "basically", "suddenly", "literally", "quite", "somewhat", "totally",
  "completely", "simply", "truly", "rather", "definitely", "certainly"];
const HEDGES = ["seemed to", "seems to", "sort of", "kind of", "a little", "a bit", "somehow", "perhaps", "slightly",
  "appeared to", "more or less", "in a way", "almost as if", "it was as if"];
const FILTERS = ["saw", "see", "sees", "seeing", "heard", "hear", "hears", "hearing", "felt", "feel", "feels", "feeling",
  "noticed", "notice", "notices", "realised", "realized", "realise", "realize", "watched", "watch", "watches", "looked",
  "seemed", "wondered", "thought", "knew", "decided", "smelled", "smelt", "tasted", "observed", "sensed", "remembered", "recalled"];
const BE = "am|is|are|was|were|be|been|being|get|gets|got|getting";
const IRREGULAR = ["arisen", "awoken", "beaten", "become", "begun", "bent", "bitten", "blown", "bought", "brought", "broken",
  "built", "burnt", "caught", "chosen", "done", "drawn", "driven", "eaten", "fallen", "fed", "felt", "fought", "found",
  "forbidden", "forgotten", "forgiven", "frozen", "given", "gone", "grown", "held", "hidden", "hit", "hung", "hurt", "kept",
  "known", "laid", "led", "left", "lent", "lost", "made", "meant", "met", "paid", "put", "read", "ridden", "rung", "risen",
  "run", "said", "seen", "sold", "sent", "set", "shaken", "shot", "shown", "shut", "slain", "sought", "spent", "spoken",
  "spun", "stolen", "struck", "stung", "sworn", "swept", "taken", "taught", "thrown", "told", "torn", "thought", "understood",
  "woken", "won", "worn", "woven", "written", "wound", "withdrawn"];
// Past participles that usually work as adjectives after "be".
const ADJECTIVAL = new Set(["tired", "interested", "excited", "worried", "scared", "bored", "married", "pleased", "surprised",
  "amazed", "annoyed", "confused", "disappointed", "embarrassed", "exhausted", "frightened", "terrified", "relieved", "shocked",
  "satisfied", "determined", "convinced", "concerned", "involved", "used", "supposed", "aged", "beloved", "crowded", "dressed",
  "engaged", "finished", "gone", "lost", "prepared", "related", "tangled", "tied", "trapped", "wounded", "armed", "naked",
  "learned", "ashamed", "alarmed", "amused", "astonished", "delighted", "depressed", "distracted", "drunk", "fascinated",
  "horrified", "intrigued", "irritated", "overwhelmed", "puzzled", "stunned", "startled", "stressed", "thrilled", "upset"]);
const STOP = new Set(("a an the and or but if then so as at by for from in into of off on onto out over to up with without " +
  "is am are was were be been being have has had having do does did done will would shall should can could may might must " +
  "i me my mine myself you your yours yourself he him his himself she her hers herself it its itself we us our ours ourselves " +
  "they them their theirs themselves this that these those there here what which who whom whose when where why how " +
  "not no nor all any both each few more most other some such only own same than too very just also again still even " +
  "back down now then once about above after against before below between through during under until while " +
  "one two three said says say asked ask like know knew get got go went come came see saw make made take took look looked " +
  "way thing things something nothing anything everything someone anyone everyone time well yes oh").split(" "));

// My own phrase lists. Each: [pattern (whole words), suggestion or ""].
const POSS = "(?:his|her|their|my|your|its|our)";
const SELF = "(?:himself|herself|themselves|myself|yourself|ourselves)";
const REDUNDANCIES = [
  [`nodded ${POSS} heads?`, "nodded"], [`shook ${POSS} heads? (?:no)`, "shook her head"], [`shrugged ${POSS} shoulders?`, "shrugged"],
  [`blinked ${POSS} eyes`, "blinked"], [`clapped ${POSS} hands`, "clapped"], [`thought to ${SELF}`, "thought"],
  [`(?:whispered|murmured) (?:quietly|softly)`, "whispered"], [`(?:shouted|yelled|screamed) loudly`, "shouted"],
  ["past history", "history"], ["end result", "result"], ["free gift", "gift"], ["close proximity", "proximity"],
  ["each and every", "each"], ["final outcome", "outcome"], ["advance warning", "warning"], ["added bonus", "bonus"],
  ["unexpected surprise", "surprise"], ["join(?:ed)? together", "joined"], ["merged? together", "merged"],
  ["return(?:ed)? back", "returned"], ["revert(?:ed)? back", "reverted"], ["repeat(?:ed)? again", "repeated"],
  ["in order to", "to"], ["at this point in time", "now"], ["the reason why", "the reason"], ["absolutely essential", "essential"],
  ["brief moment", "moment"], ["new innovation", "innovation"], ["very unique", "unique"], ["plan(?:ned)? ahead", "planned"],
  ["postponed? until later", "postponed"], ["rose up", "rose"], ["sum total", "total"], ["true fact", "fact"],
  ["unintended mistake", "mistake"], ["completely (?:destroyed|surrounded|finished|empty|full)", ""], ["exact same", "same"],
  ["first began", "began"], ["still remains?", "remains"], ["ATM machine", "ATM"], ["PIN number", "PIN"],
  ["(?:sat|crouched|knelt) down on", ""], ["descended down", "descended"], ["ascended up", "ascended"],
  [`(?:saw|watched) with ${POSS} (?:own )?eyes`, "saw"], [`heard with ${POSS} (?:own )?ears`, "heard"],
];
const CLICHES = [
  `${POSS} heart (?:pounded|hammered|raced) in ${POSS} chest`, `let out a breath (?:he|she|they|I) (?:didn't|did not) know (?:he|she|they|I) (?:was|were|had been) holding`,
  `${POSS} blood ran cold`, "time stood still", `a shiver (?:ran|went) down ${POSS} spine`, "all hell broke loose",
  "at the end of the day", "dead as a doornail", "cold as ice", "in the nick of time", "calm before the storm",
  "only time will tell", `every fib(?:re|er) of ${POSS} being`, "in the blink of an eye", "avoid (?:it )?like the plague",
  "as luck would have it", "a deafening silence", "an eerie silence", "silence was deafening", "out of the blue",
  "head over heels", "the last straw", "against all odds", "few and far between", "scared to death", "frozen in fear",
  "white as a sheet", "pale as a ghost", "quiet as a mouse", "light as a feather", "dark and stormy night",
  "the crack of dawn", "at the crack of dawn", "ignorance is bliss", "it was all a dream", "little did (?:he|she|they|I) know",
  "for all intents and purposes", "a matter of life and death", "when all was said and done", "sent shivers down",
  `${POSS} jaw dropped`, `${POSS} eyes widened in (?:shock|surprise|horror)`, "rooted to the spot", "stopped dead in (?:his|her|their|my) tracks",
  "heart of gold", "tip of the iceberg", "lump in (?:his|her|their|my) throat", "butterflies in (?:his|her|their|my) stomach",
  "the world (?:seemed to )?stop(?:ped)? (?:turning|spinning)", "like a moth to a flame", "plenty of fish in the sea",
  "as old as time", "a twist of fate", "the rest is history", "brave new world", "in the dead of night",
  "cut to the chase", "bite the bullet", "a perfect storm", "blood-curdling scream", "hit the ground running",
];
const SPELLINGS = [
  // [en-GB form, en-US form] as stems; whole words, with common endings.
  ["realis", "realiz"], ["recognis", "recogniz"], ["organis", "organiz"], ["apologis", "apologiz"], ["emphasis", "emphasiz"],
  ["criticis", "criticiz"], ["memoris", "memoriz"], ["sympathis", "sympathiz"], ["summaris", "summariz"], ["prioritis", "prioritiz"],
  ["characteris", "characteriz"], ["authoris", "authoriz"], ["civilis", "civiliz"], ["paralys", "paralyz"], ["analys", "analyz"],
  ["colour", "color"], ["favour", "favor"], ["honour", "honor"], ["harbour", "harbor"], ["neighbour", "neighbor"],
  ["armour", "armor"], ["behaviour", "behavior"], ["rumour", "rumor"], ["vigour", "vigor"], ["valour", "valor"],
  ["grey", "gray"], ["centre", "center"], ["theatre", "theater"], ["metre", "meter"], ["sombre", "somber"], ["spectre", "specter"],
  ["travell", "travel"], ["cancell", "cancel"], ["defence", "defense"], ["offence", "offense"], ["pretence", "pretense"],
  ["towards", "toward"], ["amongst", "among"], ["whilst", "while"], ["okay", "OK"],
];

// One regular expression per list (literal phrases, so no runaway matching).
const rx = (list, flags = "giu") => new RegExp(W(list.join("|")), flags);
const RX = {
  doubled: new RegExp(`(?<![${L}'’])([${L}]+(?:['’][${L}]+)?)\\s+\\1(?![${L}])`, "giu"),
  filler: rx(FILLERS), hedge: rx(HEDGES), filter: rx(FILTERS),
  began: new RegExp(W("(?:start|begin|began|begun|starts|started|begins|starting|beginning) to [\\p{L}]+"), "giu"),
  passive: new RegExp(W(`(?:${BE})(?: (?:not|never|just|already|also|then|being))? ((?:[\\p{L}]+ed)|${IRREGULAR.join("|")})(?: by)?`), "giu"),
  speechThenAdverb: new RegExp(W(`(?:${SPEECH.join("|")}|${BOOKISMS.join("|")})(?: [\\p{L}]+){0,2} ([\\p{L}]+ly)`), "giu"),
  adverbThenSpeech: new RegExp(W(`([\\p{L}]+ly) (?:${SPEECH.join("|")}|${BOOKISMS.join("|")})`), "giu"),
  bookism: rx(BOOKISMS),
  plainTag: rx(SPEECH),
  redundancy: REDUNDANCIES.map(([p, s]) => [new RegExp(W(p), "giu"), s]),
  cliche: new RegExp(W(CLICHES.join("|")), "giu"),
};

// ---------------------------------------------------------------- dialogue

const OPENERS = new Set(["“", "\"", "‘", "'", "«"]);
const PAIRS = { "“": "”", "\"": "\"", "‘": "’", "'": "’", "«": "»" };
const isLetter = (c) => !!c && /[\p{L}\p{N}]/u.test(c);
const isSpaceish = (c) => !c || /[\s(\[—–\-‘“"«]/u.test(c);

// The ranges inside quotation marks, for a paragraph. A quote that isn't closed runs
// to the end of the paragraph (a speech that goes on into the next one). Single
// quotes are told from apostrophes by what's around them: ‘ after a space opens,
// ’ before a space or punctuation (and not between letters) closes.
export function dialogueRanges(text) {
  const out = [];
  let open = -1, closer = "";
  for (let i = 0; i < text.length; i++) {
    const c = text[i], prev = text[i - 1], next = text[i + 1];
    if (open < 0) {
      if (OPENERS.has(c) && isSpaceish(prev) && next && !/\s/.test(next)) {
        if ((c === "'" || c === "‘") && !(isLetter(next) || /[“"…—]/.test(next))) continue;
        open = i; closer = PAIRS[c];
      }
    } else {
      const closes = c === closer || (closer === "’" && c === "'") || (closer === "\"" && c === "”") || (closer === "”" && c === "\"");
      if (!closes) continue;
      if ((closer === "’") && isLetter(prev) && isLetter(next)) continue;          // don’t, the dog’s
      if ((closer === "’") && next && !/[\s.,;:!?…—–)\]”"]/.test(next)) continue;  // ’cause
      out.push([open, i + 1]);
      open = -1;
    }
  }
  if (open >= 0) out.push([open, text.length]);
  return out;
}
const inRanges = (ranges, from, to) => ranges.some(([a, b]) => from < b && to > a && from >= a && to <= b);
const touchesRanges = (ranges, from, to) => ranges.some(([a, b]) => from < b && to > a);

// --------------------------------------------------------------- sentences

const SEG = typeof Intl !== "undefined" && Intl.Segmenter ? new Intl.Segmenter("en", { granularity: "sentence" }) : null;
const ABBR = /(?:\b(?:Mr|Mrs|Ms|Dr|St|Prof|Capt|Col|Gen|Lt|Sgt|Rev|Jr|Sr|vs|etc|e\.g|i\.e)\.)\s*$/;
export function sentences(text) {
  const raw = [];
  if (SEG) for (const s of SEG.segment(text)) raw.push([s.index, s.index + s.segment.length]);
  else for (const m of text.matchAll(/[^.!?…]+(?:[.!?…]+["'’”)\]]*\s*|$)/gu)) if (m[0]) raw.push([m.index, m.index + m[0].length]);
  // "Mr. Holt" isn't two sentences.
  const out = [];
  for (const r of raw) {
    if (out.length && ABBR.test(text.slice(out[out.length - 1][0], out[out.length - 1][1]))) out[out.length - 1][1] = r[1];
    else out.push(r.slice());
  }
  return out.map(([a, b]) => {
    while (a < b && /\s/.test(text[a])) a++;
    while (b > a && /\s/.test(text[b - 1])) b--;
    return [a, b];
  }).filter(([a, b]) => b > a);
}

// --------------------------------------------------------------- the rules

export const RULE_TYPES = {
  flag: "Flag these words or phrases",
  replace: "Suggest a replacement",
  limit: "Limit how often a word is used",
  nearby: "Flag a word used again too soon",
  consistent: "Keep to one spelling or name",
  pattern: "Advanced: a pattern",
};
const MAX_PATTERN = 300;

function escapeRx(s) { return s.replace(/[.*+?^${}()|[\]\\]/g, "\\$&"); }
function termsRegex(terms, rule) {
  const list = (terms || []).map((t) => String(t).trim()).filter(Boolean);
  if (!list.length) return null;
  list.sort((a, b) => b.length - a.length);
  const alt = list.map((t) => escapeRx(t).replace(/\s+/g, "\\s+") + (rule.inflections ? "(?:s|es|ed|d|ing|ly|['’]s)?" : "")).join("|");
  const body = rule.wholeWord === false ? `(?:${alt})` : W(alt);
  return new RegExp(body, rule.caseSensitive ? "gu" : "giu");
}

// Checks a rule's fields; returns "" or why it can't be used.
export function ruleProblem(rule) {
  if (!rule || typeof rule !== "object") return "Not a rule.";
  if (!RULE_TYPES[rule.type]) return "Unknown kind of rule.";
  if (rule.type === "pattern") {
    const p = String(rule.pattern || "");
    if (!p) return "The pattern is empty.";
    if (p.length > MAX_PATTERN) return "The pattern is too long (" + MAX_PATTERN + " characters at most).";
    try { RE2JS.compile(p); } catch (e) { return "The pattern isn't valid: " + String(e.message || e).replace(/^.*?:\s*/, ""); }
    return "";
  }
  if (rule.type === "replace") return (rule.pairs || []).some((p) => p && String(p.from || "").trim()) ? "" : "Add at least one word to replace.";
  if (rule.type === "consistent") return (rule.terms || []).filter((t) => String(t).trim()).length >= 2 ? "" : "Add at least two spellings.";
  if (!(rule.terms || []).some((t) => String(t).trim())) return "Add at least one word or phrase.";
  if (rule.type === "limit" && !(rule.max >= 0)) return "Say how many times is enough.";
  if (rule.type === "nearby" && !(rule.within > 0)) return "Say how many words apart.";
  return "";
}

const compiled = new WeakMap();
function compile(rule) {
  if (compiled.has(rule)) return compiled.get(rule);
  let c = null;
  if (!ruleProblem(rule)) {
    if (rule.type === "pattern") {
      c = { re2: RE2JS.compile(rule.pattern, rule.caseSensitive ? 0 : RE2JS.CASE_INSENSITIVE) };
    } else if (rule.type === "replace") {
      const pairs = (rule.pairs || []).filter((p) => p && String(p.from || "").trim());
      c = { re: termsRegex(pairs.map((p) => p.from), rule), to: new Map(pairs.map((p) => [String(p.from).trim().toLocaleLowerCase(), String(p.to || "").trim()])) };
    } else if (rule.type === "consistent") {
      const terms = rule.terms.map((t) => String(t).trim()).filter(Boolean);
      c = { terms, res: terms.map((t) => termsRegex([t], rule)) };
    } else {
      c = { re: termsRegex(rule.terms, rule) };
    }
  }
  compiled.set(rule, c);
  return c;
}

// ------------------------------------------------------------------ engine

// blocks: [{text, pos}] (pos: where the block's text starts in the document)
// opts: {mode: "off"|"quiet"|"full", lang ("en-GB"), categories: {id: bool}, rules: [...], ignore: {key: [lower-cased text]},
//        ignoreOnce: Set, names: Set(lower-cased words), echoWindow, longSentence}
// Returns {hints: [{from, to, key, cat, rule, name, msg, why}], counts: {key: n}, slow: [rule ids]}
// "said quietly" -> "whispered": verbs that carry the adverb.
const STRONGER = { quietly: ["whispered", "murmured"], softly: ["murmured", "whispered"], loudly: ["shouted", "called"],
  angrily: ["snapped", "snarled"], sharply: ["snapped"], harshly: ["snapped"], coldly: ["snapped"], nervously: ["stammered"],
  hesitantly: ["faltered"], firmly: ["insisted"], sadly: ["sighed"], softly_: [], tearfully: ["sobbed"], happily: ["beamed"],
  urgently: ["urged"], excitedly: ["exclaimed"], bitterly: ["spat"], hoarsely: ["rasped"], mockingly: ["mocked"], gently: ["soothed"] };

// Plainer ways to say a cliché: starting points, not rewrites ($1 = the matched possessive).
const CLICHE_PLAIN = [
  [/heart (?:pounded|hammered|raced)/, ["$1 pulse jumped", "$1 heart raced"]],
  [/let out a breath/, ["breathed out", "exhaled"]],
  [/blood ran cold/, ["$1 skin prickled", "dread settled on them"]],
  [/time stood still/, ["everything paused", "nobody moved"]],
  [/shiver (?:ran|went) down/, ["a chill crept over them", "they shivered"]],
  [/all hell broke loose/, ["chaos erupted", "everything went wrong at once"]],
  [/at the end of the day/, ["in the end", "ultimately"]],
  [/dead as a doornail/, ["dead", "long dead"]],
  [/cold as ice/, ["icy", "freezing"]],
  [/in the nick of time/, ["just in time", "barely in time"]],
  [/calm before the storm/, ["a tense quiet", "a lull"]],
  [/only time will tell/, ["we'll see", "nobody knows yet"]],
  [/every fib(?:re|er) of/, ["all of $1 strength", "completely"]],
  [/in the blink of an eye/, ["instantly", "in a moment"]],
  [/like the plague/, ["at all costs", "carefully"]],
  [/as luck would have it/, ["luckily", "by chance"]],
  [/(?:deafening|eerie) silence|silence was deafening/, ["a heavy silence", "a total silence"]],
  [/out of the blue/, ["unexpectedly", "suddenly"]],
  [/head over heels/, ["deeply in love", "completely"]],
  [/the last straw/, ["the final insult", "too much"]],
  [/against all odds/, ["somehow", "improbably"]],
  [/few and far between/, ["rare", "scarce"]],
  [/scared to death/, ["terrified", "petrified"]],
  [/frozen in fear/, ["paralysed", "unable to move"]],
  [/white as a sheet|pale as a ghost/, ["pale", "ashen"]],
  [/quiet as a mouse/, ["silent", "barely making a sound"]],
  [/light as a feather/, ["weightless", "almost weightless"]],
  [/crack of dawn/, ["dawn", "first light"]],
  [/little did (?:he|she|they|i) know/, ["they had no idea", "nobody knew yet"]],
  [/for all intents and purposes/, ["in effect", "practically"]],
  [/a matter of life and death/, ["urgent", "deadly serious"]],
  [/when all was said and done/, ["in the end", "afterwards"]],
  [/jaw dropped/, ["$1 mouth fell open", "they stared"]],
  [/eyes widened/, ["$1 eyes went wide", "they stared"]],
  [/rooted to the spot|stopped dead in/, ["froze", "stopped short"]],
  [/heart of gold/, ["kind", "generous"]],
  [/tip of the iceberg/, ["only the start", "a small part of it"]],
  [/lump in/, ["$1 throat tightened", "$1 voice caught"]],
  [/butterflies in/, ["$1 stomach fluttered", "nerves"]],
  [/the world (?:seemed to )?stop/, ["everything went still", "nothing else mattered"]],
  [/moth to a flame/, ["helplessly drawn", "irresistibly"]],
  [/as old as time/, ["ancient", "age-old"]],
  [/a twist of fate/, ["by chance", "an accident"]],
  [/in the dead of night/, ["late at night", "in the small hours"]],
  [/cut to the chase/, ["get to the point", "skip ahead"]],
  [/bite the bullet/, ["face it", "get it over with"]],
  [/blood-curdling scream/, ["a terrible scream", "a shriek"]],
  [/hit the ground running/, ["started at once", "got straight to work"]],
];
function plainFor(text) {
  const low = text.toLocaleLowerCase();
  for (const [re, alts] of CLICHE_PLAIN) if (re.test(low)) return alts;
  return [];
}

// "began to run" -> "ran", "starts to walk" -> "walks", "start to laugh" -> "laugh"; null when unsure.
const IRREGULAR_PAST = { run: "ran", go: "went", come: "came", see: "saw", take: "took", give: "gave", get: "got", make: "made",
  think: "thought", feel: "felt", fall: "fell", rise: "rose", speak: "spoke", say: "said", tell: "told", know: "knew",
  grow: "grew", throw: "threw", fly: "flew", drink: "drank", sing: "sang", sink: "sank", swim: "swam", begin: "began",
  eat: "ate", write: "wrote", drive: "drove", ride: "rode", shake: "shook", break: "broke", wake: "woke", freeze: "froze",
  bleed: "bled", feed: "fed", lead: "led", leave: "left", weep: "wept", sleep: "slept", creep: "crept", keep: "kept",
  sweep: "swept", sit: "sat", stand: "stood", understand: "understood", hold: "held", find: "found", bring: "brought",
  buy: "bought", fight: "fought", seek: "sought", teach: "taught", catch: "caught", dig: "dug", swing: "swung", hang: "hung",
  shine: "shone", lose: "lost", hear: "heard", tear: "tore", wear: "wore", bear: "bore", shoot: "shot", light: "lit",
  slide: "slid", hide: "hid", bite: "bit", strike: "struck", spin: "spun", stick: "stuck", sting: "stung", draw: "drew",
  blow: "blew", forget: "forgot", choose: "chose", steal: "stole", lie: "lay", lay: "laid", cry: "cried", shut: "shut",
  put: "put", cut: "cut", let: "let", hit: "hit", hurt: "hurt", spread: "spread", burst: "burst", cast: "cast", quit: "quit",
  do: "did", have: "had", be: "was", flee: "fled", sell: "sold", send: "sent", spend: "spent", build: "built", bend: "bent",
  mean: "meant", meet: "met", pay: "paid", lend: "lent", win: "won", stink: "stank", ring: "rang", shrink: "shrank" };
function pastOf(v, isWord) {
  if (IRREGULAR_PAST[v]) return IRREGULAR_PAST[v];
  let p = /e$/.test(v) ? v + "d" : /[^aeiou]y$/.test(v) ? v.slice(0, -1) + "ied" : v + "ed";
  if (/^[^aeiou]*[aeiou][bdgmnprt]$/.test(v)) { const d = v + v.slice(-1) + "ed"; if (!isWord || isWord(d)) p = d; }   // stop -> stopped
  return !isWord || isWord(p) ? p : null;
}
function thirdOf(v) {
  if (v === "have") return "has"; if (v === "be") return "is"; if (v === "do") return "does"; if (v === "go") return "goes";
  return /(?:s|sh|ch|x|z|o)$/.test(v) ? v + "es" : /[^aeiou]y$/.test(v) ? v.slice(0, -1) + "ies" : v + "s";
}
export function beganFix(phrase, isWord) {
  const m = /^(\p{L}+) to (\p{L}+)$/u.exec(phrase);
  if (!m) return null;
  const aux = m[1].toLocaleLowerCase(), v = m[2].toLocaleLowerCase();
  if (/^(?:began|begun|started)$/.test(aux)) return pastOf(v, isWord);
  if (/^(?:begins|starts)$/.test(aux)) return thirdOf(v);
  if (/^(?:begin|start)$/.test(aux)) return v;
  return null;
}

// Filter words: advice that says what to do with this one.
function filterAdvice(word) {
  const w = word.toLocaleLowerCase();
  if (/^(?:saw|see|sees|seeing|watched|watch|watches|looked|observed|noticed|notice|notices)$/.test(w))
    return `"${word}": show what's seen instead of the seeing. "She saw the door swing open" becomes "The door swung open."`;
  if (/^(?:heard|hear|hears|hearing)$/.test(w)) return `"${word}": let the sound happen. "He heard a dog bark" becomes "A dog barked."`;
  if (/^(?:felt|feel|feels|feeling|sensed)$/.test(w)) return `"${word}": show the feeling in the body or the action. "She felt afraid" could be "Her hands shook."`;
  if (/^(?:smelled|smelt|tasted)$/.test(w)) return `"${word}": give the smell or taste itself. "He smelled smoke" becomes "Smoke hung in the air."`;
  if (/^(?:realised|realized|realise|realize|knew|remembered|recalled|thought|wondered|decided)$/.test(w))
    return `"${word}": go straight to the thought. "She realised the key was gone" becomes "The key was gone."`;
  return `"${word}" puts the reader behind the character. Could the thing itself be shown?`;
}

// "The cake was eaten by the dog." -> "The dog ate the cake." (only when the whole sentence has that shape).
const PARTICIPLE_PAST = { eaten: "ate", taken: "took", given: "gave", seen: "saw", written: "wrote", driven: "drove", ridden: "rode",
  broken: "broke", chosen: "chose", spoken: "spoke", stolen: "stole", frozen: "froze", forgotten: "forgot", forgiven: "forgave",
  hidden: "hid", bitten: "bit", beaten: "beat", blown: "blew", drawn: "drew", grown: "grew", known: "knew", thrown: "threw",
  shown: "showed", sung: "sang", begun: "began", done: "did", gone: "went", fallen: "fell", shaken: "shook", woken: "woke",
  awoken: "awoke", torn: "tore", worn: "wore", sworn: "swore", born: "bore", swum: "swam", drunk: "drank", rung: "rang", struck: "struck" };
export function activeFix(b, at, isWord) {
  const t = b.text;
  let a = at; while (a > 0 && !/[.!?…]/.test(t[a - 1])) a--;
  while (/\s/.test(t[a] || "")) a++;
  let z = at; while (z < t.length && !/[.!?…]/.test(t[z])) z++;
  const end = t[z] || "";
  const m = /^(.+?) (was|were|is|are) ([\p{L}]+) by (.+)$/u.exec(t.slice(a, z));
  if (!m || /["“”‘’]/.test(m[0]) || m[1].split(" ").length > 6 || m[4].split(" ").length > 6) return null;
  const [, subj, be, part, agent] = m;
  let verb = PARTICIPLE_PAST[part.toLocaleLowerCase()] || (/ed$/.test(part) ? part : null);
  if (!verb) return null;
  if (/^(?:is|are)$/.test(be)) {   // present: "is eaten by the dog" -> "the dog eats"
    const base = Object.entries(PARTICIPLE_PAST).find(([p]) => p === part.toLocaleLowerCase());
    if (base) return null;   // irregular presents aren't worth guessing
    verb = part.replace(/ied$/, "ies").replace(/ed$/, "s");
    if (isWord && !isWord(verb)) return null;
  }
  const lowerSubj = /^(?:The|A|An|His|Her|Their|My|Our|Its|This|That|These|Those|Some|Every|Each)\b/.test(subj) ? subj[0].toLocaleLowerCase() + subj.slice(1) : subj;
  const text = agent[0].toLocaleUpperCase() + agent.slice(1) + " " + verb + " " + lowerSubj;
  return { label: `Rewrite: "${text}${end}"`, from: a, to: z, text };
}

// Long sentences: break at a joining word near the middle (", and", ", but", ";", ", which" ...).
function splitFixes(b, a, z) {
  const t = b.text, mid = (a + z) / 2, out = [];
  const re = /(,? (?:and|but|so|yet|then) |; |: |, which |, while |, when )(\p{L})/gu;
  const cands = [];
  for (const m of t.slice(a, z).matchAll(re)) {
    const at = a + m.index;
    if (at - a < 40 || z - at < 40) continue;   // both halves need some length
    cands.push({ at, m });
  }
  cands.sort((x, y) => Math.abs(x.at - mid) - Math.abs(y.at - mid));
  for (const { at, m } of cands.slice(0, 2)) {
    const joiner = m[1].trim().replace(/^,\s*/, "");
    const keep = /^(?:but|so|yet|then)$/.test(joiner) ? joiner[0].toLocaleUpperCase() + joiner.slice(1) + " " : "";
    const next = keep ? m[2] : m[2].toLocaleUpperCase();
    const which = /^which$/.test(joiner) ? "It " : /^(?:while|when)$/.test(joiner) ? joiner[0].toLocaleUpperCase() + joiner.slice(1) + " " : "";
    const text = ". " + (which || keep) + (which ? m[2] : next);
    out.push({ label: `Break at "${m[1].trim()}"`, from: at, to: at + m[0].length, text });
  }
  return out;
}

// "There was a dog barking." -> "A dog was barking."
function thereFix(b, a, z) {
  const s = b.text.slice(a, z);
  const m = /^There (was|were|is|are) ((?:a|an|the|some|two|three|four|five|many|several|no) )?([\p{L}'-]+(?: [\p{L}'-]+)?) ([\p{L}]+ing)\b/u.exec(s);
  if (!m) return null;
  const [all, be, det, noun, ving] = m;
  const start = (det || "") + noun;
  const text = start[0].toLocaleUpperCase() + start.slice(1) + " " + (det && /^no $/.test(det) ? be : be) + " " + ving;
  return { label: `"${text}"`, from: a, to: a + all.length, text };
}

// Three "She ..." sentences: "She fell. She rose." -> "She fell and rose."
function joinFix(b, from, to) {
  const t = b.text;
  const before = t.slice(0, from);
  const m = /\.\s+$/.exec(before);
  if (!m || t[to] !== " ") return null;
  // Both short: joining long sentences makes another problem.
  const prevStart = Math.max(before.slice(0, m.index).search(/[^.!?…]*$/), 0);
  const prevWords = before.slice(prevStart, m.index).trim().split(/\s+/).length;
  let end = to; while (end < t.length && !/[.!?…]/.test(t[end])) end++;
  if (prevWords > 10 || t.slice(to, end).trim().split(/\s+/).length > 10) return null;
  return { label: "Join with the sentence before", from: m.index, to: to + 1, text: " and " };
}

// The replacement in the capitalisation of what it replaces ("Very" -> "Quite", "GREY" -> "GRAY").
export function matchCase(orig, repl) {
  if (orig.length > 1 && orig === orig.toLocaleUpperCase() && orig !== orig.toLocaleLowerCase()) return repl.toLocaleUpperCase();
  if (orig[0] && orig[0] !== orig[0].toLocaleLowerCase() && repl[0] === repl[0].toLocaleLowerCase()) return repl[0].toLocaleUpperCase() + repl.slice(1);
  return repl;
}

// A fix taking a word out with one of the spaces beside it; at the start of a sentence the next word gets the capital.
export function removal(b, from, to) {
  const t = b.text, w = t.slice(from, to);
  const startsSentence = /^\s*$/.test(t.slice(0, from)) || /[.!?…]['"’”]?\s+$/u.test(t.slice(0, from)) || /[“"‘]\s*$/u.test(t.slice(0, from));
  if (startsSentence && w[0] !== w[0].toLocaleLowerCase()) {
    const m = /^[,;]?\s+(\p{L})/u.exec(t.slice(to));
    if (m) return { label: `Remove "${w}"`, from, to: to + m[0].length, text: m[1].toLocaleUpperCase() };
  }
  let a = from, z = to;
  if (t[z] === ",") z++;
  if (t[a - 1] === " ") a--; else if (t[z] === " ") z++;
  return { label: `Remove "${w}"`, from: a, to: z, text: "" };
}

export function analyse(blocks, opts) {
  const hints = [], counts = {}, slow = [];
  const mode = opts.mode || "off";
  if (mode === "off" && !(opts.rules || []).length) return { hints, counts, slow };
  const enabled = (id) => {
    if (mode === "off") return false;
    const c = CATEGORY[id];
    const on = opts.categories && id in opts.categories ? !!opts.categories[id] : c.on;
    return on && (mode === "full" || c.essential);
  };
  const ignore = opts.ignore || {};
  const ignoreOnce = opts.ignoreOnce || new Set();
  const names = opts.names || new Set();
  const echoWindow = opts.echoWindow || 50;
  const longAt = opts.longSentence || 40;

  // Fixes offered on a hint's card: [{label, from, to, text}] in the block's offsets, made absolute here
  // (with the text they replace, so a fix is only applied if that text is still there).
  const fixesIn = (b, list) => (list || []).filter(Boolean).map((f) => ({ label: f.label, text: f.text, from: b.pos + f.from, to: b.pos + f.to, orig: b.text.slice(f.from, f.to) }));
  const add = (b, from, to, key, extra) => {
    if (extra && extra.fixes) extra = { ...extra, fixes: fixesIn(b, extra.fixes) };
    const text = b.text.slice(from, to);
    const low = text.toLocaleLowerCase().replace(/\s+/g, " ");
    if ((ignore[key] || []).includes(low)) return;
    if (ignoreOnce.has(key + "|" + low + "|" + b.text.slice(Math.max(0, from - 24), to + 24))) return;
    hints.push({ from: b.pos + from, to: b.pos + to, key, text, context: b.text.slice(Math.max(0, from - 24), to + 24), ...extra });
    counts[key] = (counts[key] || 0) + 1;
  };
  const cat = (b, from, to, id, msg, fixes) => add(b, from, to, "cat:" + id, { cat: id, name: CATEGORY[id].name, msg, why: CATEGORY[id].why, fixes });
  const swap = (b, from, to, text, label) => ({ label: label || `Change to "${text}"`, from, to, text: matchCase(b.text.slice(from, to), text) });

  // Each block's dialogue, once.
  for (const b of blocks) b.dialogue = dialogueRanges(b.text);
  const narration = (b, from, to) => !touchesRanges(b.dialogue, from, to);

  const tagUses = [];   // fancy dialogue tags, judged once the whole chapter is read
  let plainTags = 0;    // said, asked and the like
  // Paragraph-level checks.
  for (const b of blocks) {
    const t = b.text;
    if (enabled("doubled"))
      for (const m of t.matchAll(RX.doubled)) {
        const w = m[1].toLocaleLowerCase();
        if (/^\d+$/.test(w) || w === "had" || w === "that") continue;
        cat(b, m.index, m.index + m[0].length, "doubled", `"${m[1]}" twice in a row.`,
          [{ label: "Remove the repeat", from: m.index, to: m.index + m[0].length, text: m[1] }]);
      }
    if (enabled("redundancy"))
      for (const [re, s] of RX.redundancy)
        for (const m of t.matchAll(re)) if (narration(b, m.index, m.index + m[0].length))
          cat(b, m.index, m.index + m[0].length, "redundancy", s ? `Maybe just "${s}".` : "Part of this may go without saying.",
            s ? [swap(b, m.index, m.index + m[0].length, s, `Change to "${s}"`)] : []);
    if (enabled("cliche"))
      for (const m of t.matchAll(RX.cliche)) if (narration(b, m.index, m.index + m[0].length))
      {
        const poss = ((/\b(his|her|their|my|your|its|our)\b/i.exec(m[0]) || ["", "their"])[1]).toLocaleLowerCase();
        const alts = plainFor(m[0]).map((p) => p.replace(/\$1/g, poss));
        cat(b, m.index, m.index + m[0].length, "cliche", "A familiar phrase. Could it be said in your own way?",
          alts.map((p) => swap(b, m.index, m.index + m[0].length, p, `"${p}"`)));
      }
    // Dialogue tags: speech verbs in narration next to some dialogue.
    if (b.dialogue.length && (enabled("tagAdverb") || enabled("bookism"))) {
      const nearDialogue = (from, to) => b.dialogue.some(([a, z]) => (from >= z && from - z <= 40) || (to <= a && a - to <= 40));
      if (enabled("tagAdverb"))
        for (const re of [RX.speechThenAdverb, RX.adverbThenSpeech])
          for (const m of t.matchAll(re)) {
            const adv = m[1].toLocaleLowerCase();
            if (NOT_ADVERBS.has(adv) || !narration(b, m.index, m.index + m[0].length) || !nearDialogue(m.index, m.index + m[0].length)) continue;
            const at = m.index + m[0].toLocaleLowerCase().lastIndexOf(adv);
            const from = re === RX.adverbThenSpeech ? m.index : at;
            const verbs = STRONGER[adv] || [];
            const tag = /^(?:said|says)$/i.test(re === RX.adverbThenSpeech ? m[0].slice(adv.length + 1) : m[0].split(" ")[0]) ? m : null;
            const fx = [removal(b, from, from + adv.length)];
            if (tag) for (const v of verbs) fx.push(swap(b, m.index, m.index + m[0].length, v, `"${v}"`));
            cat(b, from, from + adv.length, "tagAdverb",
              verbs.length && tag ? `"${m[0]}": a verb can do the adverb's work (${verbs.map((v) => `"${v}"`).join(", ")}), or show it in what's said.`
                                  : `"${m[1]}" on a dialogue tag. Can the words themselves, or an action, show how it was said?`, fx);
          }
      if (enabled("bookism")) {
        for (const m of t.matchAll(RX.bookism))
          if (narration(b, m.index, m.index + m[0].length) && nearDialogue(m.index, m.index + m[0].length)) {
            const q = b.dialogue.find(([a, z]) => z <= m.index && m.index - z <= 40) || b.dialogue.find(([a]) => a >= m.index);
            tagUses.push({ b, m, asked: !!(q && /\?['"’”]?\s*$/u.test(t.slice(q[0], q[1]))) });
          }
        for (const m of t.matchAll(RX.plainTag))
          if (narration(b, m.index, m.index + m[0].length) && nearDialogue(m.index, m.index + m[0].length)) plainTags++;
      }
    }
    if (enabled("filler")) {
      const found = [...t.matchAll(RX.filler)].filter((m) => narration(b, m.index, m.index + m[0].length));
      if (found.length >= 2) for (const m of found) cat(b, m.index, m.index + m[0].length, "filler", `"${m[0]}" (${found.length} filler words in this paragraph).`,
        [removal(b, m.index, m.index + m[0].length)]);
    }
    if (enabled("hedge"))
      for (const m of t.matchAll(RX.hedge)) if (narration(b, m.index, m.index + m[0].length))
        cat(b, m.index, m.index + m[0].length, "hedge", `"${m[0]}" softens this.`, [removal(b, m.index, m.index + m[0].length)]);
    if (enabled("filter"))
      for (const m of t.matchAll(RX.filter)) if (narration(b, m.index, m.index + m[0].length))
        cat(b, m.index, m.index + m[0].length, "filter", filterAdvice(m[0]));
    if (enabled("began"))
      for (const m of t.matchAll(RX.began)) if (narration(b, m.index, m.index + m[0].length))
      {
        const fx = beganFix(m[0], opts.isWord);
        cat(b, m.index, m.index + m[0].length, "began", "Could the action simply happen?",
          fx ? [swap(b, m.index, m.index + m[0].length, fx, `Change to "${fx}"`)] : []);
      }
    if (enabled("passive"))
      for (const m of t.matchAll(RX.passive)) {
        if (ADJECTIVAL.has(m[1].toLocaleLowerCase()) || !narration(b, m.index, m.index + m[0].length)) continue;
      {
        const fx = activeFix(b, m.index, opts.isWord);
        cat(b, m.index, m.index + m[0].length, "passive",
          fx ? `Passive: "${fx.text}" names who did it first.` : "Possibly passive. If it matters who did it, start with them; if not, it's fine.", fx ? [fx] : []);
      }
      }
    if (enabled("long") || enabled("thereIs"))
      for (const [a, z] of sentences(t)) {
        if (!narration(b, a, z)) continue;
        const s = t.slice(a, z);
        if (enabled("long")) {
          const n = countWords(s);
          if (n > longAt) {
            const fx = splitFixes(b, a, z);
            cat(b, a, z, "long", `${n} words in one sentence${n > longAt * 1.5 ? " (very long)" : ""}.` +
              (fx.length ? " It could break here:" : " Look for a place where one thought ends and the next begins."), fx);
          }
        }
        if (enabled("thereIs")) {
          const m = /^There (is|are|was|were|had been|will be)\b/u.exec(s);
          if (m) {
            const fx = thereFix(b, a, z);
            cat(b, a, a + m[0].length, "thereIs", fx ? `Starts with "${m[0]}". The subject could lead:` : `Starts with "${m[0]}". Could the thing that's there start the sentence, doing something?`, fx ? [fx] : []);
          }
        }
      }
  }

  // Fancy dialogue tags: only when it's overdone. A tag that's an action ("she smiled") always;
  // otherwise when fancy tags outnumber said and asked (and there are at least five), or the same
  // one keeps coming back (its third use on). One "he screamed" in a tense scene is left alone.
  if (tagUses.length) {
    const many = tagUses.length >= 5 && tagUses.length > plainTags;
    const seenTag = new Map();
    for (const { b, m, asked } of tagUses) {
      const word = m[0].toLocaleLowerCase();
      const n = (seenTag.get(word) || 0) + 1;
      seenTag.set(word, n);
      const action = ACTION_TAGS.has(word);
      if (!action && !many && n < 3) continue;
      const fx = [swap(b, m.index, m.index + m[0].length, "said")];
      if (asked) fx.unshift(swap(b, m.index, m.index + m[0].length, "asked"));
      const msg = action
        ? `"${m[0]}" can't carry words: nobody can ${ACTION_TAGS.get(word)} a sentence. Use "said", or end the line and make it an action: “Fine.” She ${m[0]}.`
        : n >= 3 ? `"${m[0]}" is the tag for the ${ordinal(n)} time in this chapter. Readers start to notice it.`
        : `${tagUses.length} fancy tags and ${plainTags} plain ones (said, asked) in this chapter. "Said" disappears; these don't.`;
      cat(b, m.index, m.index + m[0].length, "bookism", msg, fx);
    }
  }

  // Repeated sentence starts, across the chapter's narration.
  if (enabled("starts")) {
    let last = "", run = [];
    const flush = () => { if (run.length >= 3) for (const r of run.slice(2)) {
      const fx = joinFix(r.b, r.from, r.to);
      cat(r.b, r.from, r.to, "starts", `${run.length} sentences in a row start with "${r.word}". Join two, or open this one with something else (a place, a time, the action).`, fx ? [fx] : []);
    } };
    for (const b of blocks)
      for (const [a, z] of sentences(b.text)) {
        if (!narration(b, a, z)) { flush(); last = ""; run = []; continue; }
        const m = /^[\p{L}\p{N}]+(?:['’][\p{L}]+)?/u.exec(b.text.slice(a, z));
        const word = m ? m[0] : "";
        const low = word.toLocaleLowerCase();
        if (low && low === last) run.push({ b, from: a, to: a + word.length, word });
        else { flush(); run = word ? [{ b, from: a, to: a + word.length, word }] : []; last = low; }
      }
    flush();
  }

  // Echoes: the same distinctive word again within `echoWindow` words of narration.
  if (enabled("echo")) {
    const seen = new Map();   // stem -> word index
    const where = new Map();  // stem -> the earlier use's document range
    let index = 0;
    for (const b of blocks)
      for (const m of b.text.matchAll(WORD)) {
        index++;
        const raw = m[0], low = raw.toLocaleLowerCase().replace(/['’]s$/, "");
        if (low.length < 4 || STOP.has(low) || names.has(low) || /^\d/.test(low)) continue;
        if (!narration(b, m.index, m.index + raw.length)) continue;
        const stem = low.length > 5 ? low.replace(/(?:ing|ed|es|s|ly)$/, "") : low;
        const before = seen.get(stem);
        if (before !== undefined && index - before <= echoWindow)
        {
          cat(b, m.index, m.index + raw.length, "echo", `"${raw}" was also used ${index - before} words earlier.`);
          hints[hints.length - 1] && hints[hints.length - 1].key === "cat:echo" && (hints[hints.length - 1].earlier = where.get(stem));
        }
        seen.set(stem, index);
        where.set(stem, { from: b.pos + m.index, to: b.pos + m.index + raw.length });
      }
  }

  // Mixed spellings and dashes: the less-used form of each pair is marked.
  if (enabled("spelling")) {
    const all = [];   // {b, from, to, group, form}
    for (const [gb, us] of SPELLINGS) {
      const re = new RegExp(W(`(${escapeRx(gb)}|${escapeRx(us)})(?:e|es|ed|ing|ation|ations|er|ers|s|ist|ists|ism)?`), "giu");
      for (const b of blocks) for (const m of b.text.matchAll(re))
        all.push({ b, from: m.index, to: m.index + m[0].length, group: gb, form: m[1].toLocaleLowerCase() === gb.toLocaleLowerCase() ? "gb" : "us", text: m[0],
          root: m[1].length, gb, us });
    }
    for (const b of blocks) {
      for (const m of b.text.matchAll(/(?<=\S)—(?=\S)/gu)) all.push({ b, from: m.index, to: m.index + 1, group: "dash", form: "closed em dash", text: "—" });
      for (const m of b.text.matchAll(/ – /gu)) all.push({ b, from: m.index + 1, to: m.index + 2, group: "dash", form: "spaced en dash", text: "–" });
      for (const m of b.text.matchAll(/ — /gu)) all.push({ b, from: m.index + 1, to: m.index + 2, group: "dash", form: "spaced em dash", text: "—" });
    }
    const groups = new Map();
    for (const x of all) { if (!groups.has(x.group)) groups.set(x.group, []); groups.get(x.group).push(x); }
    for (const [group, xs] of groups) {
      const byForm = new Map();
      for (const x of xs) byForm.set(x.form, (byForm.get(x.form) || 0) + 1);
      if (byForm.size < 2) continue;
      // The more used form stays; on a tie, the book's language decides (British unless it's en-US).
      const preferred = group === "dash" ? xs[0].form : /^en-US/i.test(opts.lang || "") ? "us" : "gb";
      const main = [...byForm.entries()].sort((a, b) => b[1] - a[1] || (a[0] === preferred ? -1 : b[0] === preferred ? 1 : 0))[0][0];
      for (const x of xs) if (x.form !== main) {
        const what = group === "dash" ? `A ${x.form}; this chapter mostly uses ${main}es.`
          : `"${x.text}": this chapter mostly uses the ${main === "gb" ? "British" : "American"} spelling.`;
        let fix = null;
        if (group === "dash") {
          const look = { "closed em dash": "—", "spaced en dash": " – ", "spaced em dash": " — " };
          const spaced = x.form !== "closed em dash";
          fix = { label: `Use a ${main}`, from: spaced ? x.from - 1 : x.from, to: spaced ? x.to + 1 : x.to, text: look[main] };
        } else {
          const other = main === "gb" ? x.gb : x.us;
          fix = swap(x.b, x.from, x.to, other + x.text.slice(x.root));
        }
        cat(x.b, x.from, x.to, "spelling", what, [fix]);
      }
    }
  }

  // The writer's own rules.
  const words = blocks.reduce((n, b) => n + countWords(b.text), 0);
  for (const rule of opts.rules || []) {
    if (rule.enabled === false) continue;
    const c = compile(rule);
    if (!c) continue;
    const key = "rule:" + rule.id;
    const where = rule.where || "all";
    const placeOk = (b, from, to) => where === "all" || (where === "narration" ? narration(b, from, to) : inRanges(b.dialogue, from, to));
    const extra = (msg) => ({ rule: rule.id, name: rule.name || RULE_TYPES[rule.type], msg, why: rule.explanation || "" });
    const message = (text, fallback) => (rule.message ? String(rule.message).replace(/%s/g, text) : fallback);
    const started = (typeof performance !== "undefined" ? performance : Date).now();
    if (rule.type === "pattern") {
      for (const b of blocks) {
        const mt = c.re2.matcher(b.text);
        let guard = 0;
        while (mt.find() && guard++ < 2000) {
          const from = mt.start(), to = mt.end();
          if (to <= from || !placeOk(b, from, to)) continue;
          add(b, from, to, key, extra(message(b.text.slice(from, to), `Matches your pattern "${rule.name || rule.pattern}".`)));
        }
      }
    } else if (rule.type === "replace") {
      for (const b of blocks) for (const m of b.text.matchAll(c.re)) {
        if (!placeOk(b, m.index, m.index + m[0].length)) continue;
        const base = [...c.to.keys()].find((k) => m[0].toLocaleLowerCase().startsWith(k)) || m[0].toLocaleLowerCase();
        const to = c.to.get(base);
        add(b, m.index, m.index + m[0].length, key, { ...extra(message(m[0], to ? `"${m[0]}": you prefer "${to}".` : `"${m[0]}": you'd rather not use this.`)),
          fixes: to ? [swap(b, m.index, m.index + m[0].length, to)] : [removal(b, m.index, m.index + m[0].length)] });
      }
    } else if (rule.type === "flag") {
      for (const b of blocks) for (const m of b.text.matchAll(c.re))
        if (placeOk(b, m.index, m.index + m[0].length)) add(b, m.index, m.index + m[0].length, key, extra(message(m[0], `"${m[0]}" is on your list.`)));
    } else if (rule.type === "limit") {
      const found = [];
      for (const b of blocks) for (const m of b.text.matchAll(c.re)) if (placeOk(b, m.index, m.index + m[0].length)) found.push({ b, m });
      const allowed = rule.per === "1000" ? Math.max(0, Math.floor((rule.max || 0) * Math.max(1, words) / 1000)) : (rule.max || 0);
      if (found.length > allowed)
        for (const { b, m } of found.slice(allowed))
          add(b, m.index, m.index + m[0].length, key, extra(message(m[0], `Used ${found.length} times here; your limit is ${allowed}${rule.per === "1000" ? " for a chapter this long" : " a chapter"}.`)));
    } else if (rule.type === "nearby") {
      let index = 0;
      const last = new Map();
      for (const b of blocks) {
        const hits = [...b.text.matchAll(c.re)].map((m) => ({ at: m.index, m }));
        let hi = 0;
        for (const w of b.text.matchAll(WORD)) {
          index++;
          while (hi < hits.length && hits[hi].at < w.index) hi++;
          if (hi < hits.length && hits[hi].at === w.index) {
            const m = hits[hi].m, k = m[0].toLocaleLowerCase();
            if (placeOk(b, m.index, m.index + m[0].length)) {
              const prev = last.get(k);
              if (prev !== undefined && index - prev <= rule.within)
                add(b, m.index, m.index + m[0].length, key, extra(message(m[0], `"${m[0]}" again, ${index - prev} words after the last one.`)));
              last.set(k, index);
            }
          }
        }
      }
    } else if (rule.type === "consistent") {
      const found = c.terms.map(() => []);
      c.res.forEach((re, i) => { for (const b of blocks) for (const m of b.text.matchAll(re)) if (placeOk(b, m.index, m.index + m[0].length)) found[i].push({ b, m }); });
      // The first spelling listed is the one to keep, if it's used; otherwise the most used.
      let keep = found[0].length ? 0 : found.reduce((best, f, i) => (f.length > found[best].length ? i : best), 0);
      found.forEach((f, i) => { if (i !== keep) for (const { b, m } of f)
        add(b, m.index, m.index + m[0].length, key, { ...extra(message(m[0], `"${m[0]}": elsewhere it's "${c.terms[keep]}".`)),
          fixes: [swap(b, m.index, m.index + m[0].length, c.terms[keep])] }); });
    }
    const spent = (typeof performance !== "undefined" ? performance : Date).now() - started;
    if (spent > 150) slow.push(rule.id);
  }
  hints.sort((a, b) => a.from - b.from || b.to - a.to);
  return { hints, counts, slow };
}

// Flesch reading ease for the chapter's narration and dialogue together (a rough guide: it ignores meaning).
export function readingEase(blocks) {
  let words = 0, sents = 0, syll = 0;
  for (const b of blocks) {
    sents += sentences(b.text).length;
    for (const m of b.text.matchAll(WORD)) {
      words++;
      const w = m[0].toLowerCase().replace(/[^a-z]/g, "");
      if (!w) continue;
      let n = (w.replace(/(?:[^laeiouy]es|ed|[^laeiouy]e)$/, "").replace(/^y/, "").match(/[aeiouy]{1,2}/g) || []).length;
      syll += Math.max(1, n);
    }
  }
  if (!words || !sents) return null;
  return Math.max(0, Math.min(100, Math.round(206.835 - 1.015 * (words / sents) - 84.6 * (syll / words))));
}

// A new rule's id.
export function newRuleId() {
  return Array.from(crypto.getRandomValues(new Uint8Array(6)), (b) => b.toString(16).padStart(2, "0")).join("");
}

// Accepts a rule set from a file: {format:"nrstyle", version:1, rules:[...], ignore:{...}}.
// Returns {rules, ignore, problems}.
export function readRuleSet(data) {
  const problems = [];
  if (!data || typeof data !== "object" || !Array.isArray(data.rules)) return { rules: [], ignore: {}, problems: ["This isn't a NorthRay style rules file."] };
  const keep = ["id", "name", "type", "terms", "pairs", "max", "per", "within", "pattern", "where", "caseSensitive", "wholeWord",
    "inflections", "message", "explanation", "source", "enabled"];
  const rules = [];
  for (const r of data.rules.slice(0, 500)) {
    const x = {};
    for (const k of keep) if (r && k in r) x[k] = r[k];
    if (!/^[0-9a-z-]{4,40}$/i.test(String(x.id || ""))) x.id = newRuleId();
    if (Array.isArray(x.terms)) x.terms = x.terms.map(String).slice(0, 500);
    if (Array.isArray(x.pairs)) x.pairs = x.pairs.filter((p) => p && typeof p === "object").map((p) => ({ from: String(p.from || ""), to: String(p.to || "") })).slice(0, 500);
    const why = ruleProblem(x);
    if (why) { problems.push(`"${x.name || "A rule"}": ${why}`); continue; }
    rules.push(x);
  }
  const ignore = {};
  if (data.ignore && typeof data.ignore === "object")
    for (const [k, v] of Object.entries(data.ignore)) if (/^(cat|rule):[\w-]+$/.test(k) && Array.isArray(v)) ignore[k] = v.map(String).slice(0, 1000);
  return { rules, ignore, problems };
}

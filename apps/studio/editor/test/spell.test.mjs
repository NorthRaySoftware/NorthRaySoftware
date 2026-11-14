import test from "node:test";
import assert from "node:assert/strict";
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const web = path.join(path.dirname(fileURLToPath(import.meta.url)), "../../web/");
globalThis.fetch = async (u) => ({ text: async () => fs.readFileSync(web + u, "utf8") });
const sp = await import("../src/spell.js");
await new Promise((r) => sp.load("en-GB", r));

test("suggestions reach words several letters off", () => {
  assert.equal(sp.suggestions("skelingtons")[0], "skeletons");
  assert.equal(sp.suggestions("definately")[0], "definitely");
  assert.equal(sp.suggestions("Skelington")[0], "Skeleton");
});

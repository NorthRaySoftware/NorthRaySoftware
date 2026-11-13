// Checks shared/metadata for mistakes before they reach the launcher or site.
// Run: node tools/validate-metadata.mjs
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../shared/metadata');
const problems = [];
const read = (rel) => {
  try { return JSON.parse(fs.readFileSync(path.join(root, rel), 'utf8')); }
  catch (e) { problems.push(`${rel}: ${e.message}`); return null; }
};

const ids = new Set();
for (const f of fs.readdirSync(path.join(root, 'apps')).filter((f) => f.endsWith('.json'))) {
  const a = read(`apps/${f}`);
  if (!a) continue;
  if (a.schema !== 1) problems.push(`apps/${f}: schema must be 1`);
  if (!/^[a-z0-9]+(-[a-z0-9]+)*$/.test(a.id ?? '')) problems.push(`apps/${f}: invalid id`);
  if (ids.has(a.id)) problems.push(`apps/${f}: duplicate id ${a.id}`);
  ids.add(a.id);
  for (const k of ['name', 'tagline', 'description', 'status']) if (!a[k]) problems.push(`apps/${f}: missing ${k}`);
  const exe = a.launch?.executable ?? '';
  if (exe.includes('..') || exe.includes(':') || /^[\\/]/.test(exe)) problems.push(`apps/${f}: executable must stay inside the app folder`);
  if (a.accent && !/^#[0-9A-Fa-f]{6}$/.test(a.accent)) problems.push(`apps/${f}: accent must be #RRGGBB`);
}

const checkProgrammes = (rel, demoExpected) => {
  const s = read(rel);
  if (!s) return;
  if (!!s.demo !== demoExpected) problems.push(`${rel}: "demo" flag should be ${demoExpected}`);
  for (const p of s.programmes ?? []) {
    if (!['draft', 'confirmed', 'cancelled', 'postponed'].includes(p.status)) problems.push(`${rel}: ${p.id} has unknown status`);
    if (!p.startUk && !p.relative) problems.push(`${rel}: ${p.id} has no start time`);
    if (p.startUk && p.kind === 'regular-saturday') {
      const [d] = p.startUk.split('T');
      if (new Date(d + 'T12:00:00Z').getUTCDay() !== 6) problems.push(`${rel}: ${p.id} is a Saturday programme not on a Saturday`);
    }
  }
};
checkProgrammes('radio/schedule.json', false);
checkProgrammes('demo/radio-schedule.demo.json', true);
for (const f of fs.readdirSync(path.join(root, 'demo'))) {
  if (f.endsWith('.json') && !f.startsWith('profile')) {
    const d = read(`demo/${f}`);
    if (d && d.demo !== true) problems.push(`demo/${f}: demo files must set "demo": true`);
  }
}

if (problems.length) {
  console.error(problems.map((p) => '  ' + p).join('\n'));
  console.error(`\n${problems.length} problem(s) found.`);
  process.exit(1);
}
console.log(`Metadata OK: ${ids.size} application(s).`);

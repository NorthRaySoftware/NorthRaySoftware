// After `astro build`: turn root links ("/apps/", "/_astro/x.css") into
// relative ones ("../apps/index.html", "../_astro/x.css"), so the site works
// from any folder on any host, and when index.html is opened straight from disk.
import fs from 'node:fs';
import path from 'node:path';

const dist = path.resolve(process.argv[2] ?? 'dist');
let files = 0, links = 0;
function walk(dir) {
  for (const e of fs.readdirSync(dir, { withFileTypes: true })) {
    const p = path.join(dir, e.name);
    if (e.isDirectory()) walk(p);
    else if (e.name.endsWith('.html')) fix(p);
  }
}
function fix(file) {
  const rel = path.relative(path.dirname(file), dist).split(path.sep).join('/');
  const prefix = rel ? rel + '/' : './';
  const html = fs.readFileSync(file, 'utf8').replace(/\b(href|src|srcset|action)="\/(?!\/)([^"]*)"/g, (_, attr, target) => {
    links++;
    let [pathPart, hash = ''] = target.split('#');
    if (pathPart === '' || pathPart.endsWith('/')) pathPart += 'index.html';
    return `${attr}="${prefix}${pathPart}${hash ? '#' + hash : ''}"`;
  });
  fs.writeFileSync(file, html);
  files++;
}
walk(dist);
console.log(`relative-links: ${links} links in ${files} pages`);

// Usage: node tools/editor-harness.cjs <screenshot folder> [width] [height] [dark]  (needs Playwright with Chromium).
// A stand-in for Writing Studio around the real editor: serves apps/studio/web, gives the page a WebView2-like host,
// sends the view and a chapter the way the app does, and records what the editor sends back.
const { chromium } = require('playwright');
const http = require('http'), fs = require('fs'), path = require('path');
const ROOT = path.join(__dirname, '..', 'apps', 'studio', 'web');
const S = process.argv[2] || '.', W = +(process.argv[3] || 960), H = +(process.argv[4] || 760), dark = process.argv[5] === 'dark';
const srv = http.createServer((q, r) => {
  const f = path.join(ROOT, decodeURIComponent(q.url.split('?')[0]));
  fs.readFile(f, (e, d) => { if (e) { r.writeHead(404); r.end(); return; }
    const ext = path.extname(f); r.writeHead(200, {'Content-Type': {'.html':'text/html','.js':'text/javascript','.css':'text/css','.json':'application/json'}[ext] || 'application/octet-stream'}); r.end(d); });
}).listen(8766, '127.0.0.1');
const text = fs.readFileSync(path.join(__dirname, '..', 'tests', 'fixtures', 'paste', 'tom.txt'), 'utf8');
const paras = text.split(/\n\s*\n/).map(s => s.replace(/\s+/g, ' ').trim()).filter(s => s.length > 60).slice(0, 14);
const doc = { type: 'doc', content: paras.map((p, i) => ({ type: 'paragraph', attrs: { style: i === 0 ? 'first' : 'body' }, content: [{ type: 'text', text: p }] })) };
(async () => {
  const b = await chromium.launch();
  const page = await b.newPage({ viewport: { width: W, height: H } });
  const out = [];
  await page.exposeFunction('__post', m => out.push(m));
  await page.addInitScript(() => {
    const ls = [];
    window.chrome = { webview: { postMessage: m => window.__post(m), addEventListener: (t, f) => ls.push(f) } };
    window.__send = m => ls.forEach(f => f({ data: m }));
  });
  page.on('pageerror', e => console.log('ERR', e.message));
  await page.goto('http://127.0.0.1:8766/editor.html');
  await page.waitForTimeout(400);
  await page.evaluate(([doc, dark]) => {
    window.__send({ type: 'view', settings: {}, names: [{ id: 'e1', name: 'Tom', names: ['Tom','Thomas'], kind: 'Character' }, { id: 'e2', name: 'Astrid', names: ['Astrid'], kind: 'Character' }, { id: 'e3', name: 'Elandroth', names: ['Elandroth'], kind: 'Place' }], bookFont: 'Crimson Text' });
    window.__send({ type: 'load', id: 'n1', title: 'Chapter One: The Forge', doc, placeholder: 'Start writing.', language: 'en-GB', dark });
  }, [doc, dark]);
  await page.waitForTimeout(1200);
  await page.screenshot({ path: S + '/h-' + W + (dark ? '-dark' : '') + '.png' });
  await page.click('#page p:nth-of-type(2)');
  await page.keyboard.type(' A new sentence typed here.');
  await page.waitForTimeout(1500);
  await page.screenshot({ path: S + '/h-' + W + '-typed.png' });
  for (const [sel, name] of [['#viewbtn, [data-label="Aa"], #aa', 'aa'], ['#hints', 'hints']]) {
    const el = await page.$(sel);
    if (el) { await el.click(); await page.waitForTimeout(500); await page.screenshot({ path: S + '/h-' + W + '-' + name + '.png' }); await page.keyboard.press('Escape'); await page.mouse.click(5, H - 5); await page.waitForTimeout(300); }
  }
  console.log('out:', [...new Set(out.map(m => m.type))].join(','));
  await b.close(); srv.close();
})();

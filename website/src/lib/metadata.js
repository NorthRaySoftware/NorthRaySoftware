// Reads the same shared metadata the Windows launcher ships with, so the
// website and launcher can't disagree about applications.
// Files are pulled in at build time through Vite's import.meta.glob.
const appFiles = import.meta.glob('../../../shared/metadata/apps/*.json', { eager: true, import: 'default' });
import identity from '../../../shared/metadata/brand/identity.json';
const shotLists = import.meta.glob('../../../shared/metadata/screenshots/*.json', { eager: true, import: 'default' });
const shotFiles = import.meta.glob('../../../shared/metadata/screenshots/*/*.png', { eager: true, query: '?url', import: 'default' });
const noteFiles = import.meta.glob('../../../shared/metadata/releases/*.json', { eager: true, import: 'default' });
// Written by tools/make-installers.sh; absent until installers have been built.
const releaseFiles = import.meta.glob('../generated/releases.json', { eager: true, import: 'default' });

// Catalogue order.
const order = ['writing-studio', 'browser', 'files', 'capture'];
function allApps() {
  return Object.values(appFiles).sort((a, b) => (order.indexOf(a.id) + 99) % 99 - (order.indexOf(b.id) + 99) % 99);
}
export function loadApps() {
  return allApps();
}

export function brand() {
  return identity;  // developer name, contact email: the one central setting
}

export function releaseNotes(id) {
  const f = Object.entries(noteFiles).find(([k]) => k.endsWith('/' + id + '.json'));
  return f ? f[1].releases : [];
}

export const statusLabel = {
  'in-development': 'In development',
  preview: 'Preview',
  available: 'Available',
  retired: 'Retired',
};

// The installers that were actually built: name, file, size and SHA-256.
export function release() {
  return Object.values(releaseFiles)[0] ?? null;
}

export function formatSize(bytes) {
  return (bytes / (1024 * 1024)).toFixed(1) + ' MB';
}

// Screenshots of an app: the same files and captions the Launcher shows.
export function screenshots(id) {
  const list = Object.entries(shotLists).find(([k]) => k.endsWith('/' + id + '.json'))?.[1]?.screenshots ?? [];
  return list
    .map((s) => ({ url: Object.entries(shotFiles).find(([k]) => k.endsWith('/screenshots/' + s.file))?.[1], caption: s.caption ?? '' }))
    .filter((s) => s.url);
}

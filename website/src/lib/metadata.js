// Reads the same shared metadata the Windows launcher ships with, so the
// website and launcher can't disagree about applications or the schedule.
// Files are pulled in at build time through Vite's import.meta.glob.
const appFiles = import.meta.glob('../../../shared/metadata/apps/*.json', { eager: true, import: 'default' });
import schedule from '../../../shared/metadata/radio/schedule.json';
import station from '../../../shared/metadata/radio/station.json';
import identity from '../../../shared/metadata/brand/identity.json';
const shotLists = import.meta.glob('../../../shared/metadata/screenshots/*.json', { eager: true, import: 'default' });
const shotFiles = import.meta.glob('../../../shared/metadata/screenshots/*/*.png', { eager: true, query: '?url', import: 'default' });
const noteFiles = import.meta.glob('../../../shared/metadata/releases/*.json', { eager: true, import: 'default' });
// Written by tools/make-installers.sh; absent until installers have been built.
const releaseFiles = import.meta.glob('../generated/releases.json', { eager: true, import: 'default' });

// Catalogue order: the Writing Studio, CJ Radio, then the optional community app.
const order = ['writing-studio', 'cj-radio', 'cj-philip-community'];
function allApps() {
  return Object.values(appFiles).sort((a, b) => (order.indexOf(a.id) + 99) % 99 - (order.indexOf(b.id) + 99) % 99);
}
// Apps associated with CJ Philip (CJ Philip Community and the apps inside it)
// are an optional download in NorthRay Launcher. The website advertises
// NorthRay's own apps and mentions CJ Philip's only briefly.
export function isCjPhilipApp(a) {
  return a.credits?.associatedCreator === 'CJ Philip';
}
export function loadApps() {
  return allApps().filter((a) => !isCjPhilipApp(a));
}
export function cjPhilipApps() {
  return allApps().filter(isCjPhilipApp);
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

// The real schedule only. Demo programmes never appear on the website.
// Live status isn't decided here: a static build can't know whether a
// broadcast is on air. That will come from the broadcast service at runtime.
export function confirmedProgrammes() {
  return (schedule.programmes ?? []).filter((p) => p.status === 'confirmed');
}

// The installers that were actually built: name, file, size and SHA-256.
export function release() {
  return Object.values(releaseFiles)[0] ?? null;
}

export function radioLinks() {
  return {
    twitch: station.twitchUrl || '',
    youtube: (station.youtubeUrl || '').replace(/\/live$/, ''),
    discord: station.callIn?.guestUrl || '',
    calendar: station.calendar?.subscribeUrl || '',
    calendarIcs: station.calendar?.icsUrl || '',
    minimumAge: station.callIn?.minimumAge ?? 16,
  };
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

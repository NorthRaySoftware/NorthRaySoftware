#!/usr/bin/env bash
# Builds the NorthRay installers with NSIS:
#   NorthRayLauncher-Setup, WritingStudio-Setup, CJRadio-Setup, CJPhilipCommunity-Setup
# then writes catalogue.json (version, size and SHA-256 of each; what NorthRay
# Launcher reads to install and update) and copies everything into the website.
#   tools/build-windows.sh && tools/make-installers.sh
set -euo pipefail
cd "$(dirname "$0")/.."
ROOT=$(pwd)
VERSION=$(tr -d '[:space:]' < VERSION)
DEVELOPER=$(node -e "process.stdout.write(require('./shared/metadata/brand/identity.json').developer)")
BUILD=build/windows/launcher
OUT=build/installers
rm -rf "$OUT" && mkdir -p "$OUT/stage/launcher"

# NorthRay Launcher on its own: the executable and its metadata, no apps.
cp "$BUILD/NorthRayLauncher.exe" "$OUT/stage/launcher/"
cp -r "$BUILD/data" "$OUT/stage/launcher/"

makensis -V2 -DDEVELOPER="$DEVELOPER" -DAPP_VERSION="$VERSION" -DSRC_DIR="$ROOT/$OUT/stage/launcher" \
  -DICON="$ROOT/launcher/resources/northray-launcher.ico" -DART="$ROOT/installer/art/launcher.bmp" \
  -DOUT_FILE="$ROOT/$OUT/NorthRayLauncher-Setup-$VERSION.exe" installer/launcher.nsi

app() {  # id name exe icon file-stem [extra makensis flags]
  makensis -V2 ${6:-} -DDEVELOPER="$DEVELOPER" -DAPP_ID="$1" -DAPP_NAME="$2" -DAPP_EXE="$3" -DAPP_VERSION="$VERSION" \
    -DSRC_DIR="$ROOT/$BUILD/packages/$1" -DICON="$ROOT/$4" -DART="$ROOT/installer/art/$1.bmp" \
    -DOUT_FILE="$ROOT/$OUT/$5-Setup-$VERSION.exe" installer/app.nsi
}
app writing-studio "NorthRay Writing Studio" WritingStudio.exe apps/studio/resources/writing-studio.ico WritingStudio
app cj-radio "CJ Radio" CJRadio.exe apps/cj-radio/resources/cj-radio.ico CJRadio -DNO_SHORTCUT
app cj-philip-community "CJ Philip Community" CJPhilipCommunity.exe apps/community/resources/cj-community.ico CJPhilipCommunity
rm -rf "$OUT/stage"

node - "$OUT" "$VERSION" <<'JS'
const fs = require('fs'), crypto = require('crypto'), path = require('path');
const [dir, version] = process.argv.slice(2);
const items = [
  ['launcher', 'NorthRay Launcher', `NorthRayLauncher-Setup-${version}.exe`],
  ['writing-studio', 'NorthRay Writing Studio', `WritingStudio-Setup-${version}.exe`],
  ['cj-radio', 'CJ Radio', `CJRadio-Setup-${version}.exe`],
  ['cj-philip-community', 'CJ Philip Community', `CJPhilipCommunity-Setup-${version}.exe`],
].map(([id, name, file]) => {
  const buf = fs.readFileSync(path.join(dir, file));
  return { id, name, version, file, url: `downloads/${file}`, size: buf.length,
           sha256: crypto.createHash('sha256').update(buf).digest('hex') };
});
const builtUtc = new Date().toISOString().replace(/\.\d+Z$/, 'Z');
// catalogue.json: what NorthRay Launcher reads. URLs are relative to it.
fs.writeFileSync(path.join(dir, 'catalogue.json'),
  JSON.stringify({ format: 'northray-catalogue', formatVersion: 1, generatedUtc: builtUtc, apps: items }, null, 2) + '\n');
// releases.json: the same, for the website's download page.
fs.writeFileSync(path.join(dir, 'releases.json'), JSON.stringify({ version, builtUtc, installers: items }, null, 2) + '\n');
JS

# Hand them to the website: installers under downloads/, the catalogue at the root.
mkdir -p website/public/downloads website/src/generated
rm -f website/public/downloads/*.exe
cp "$OUT"/*.exe website/public/downloads/
cp "$OUT/catalogue.json" website/public/catalogue.json
cp "$OUT/releases.json" website/src/generated/releases.json
ls -l "$OUT"

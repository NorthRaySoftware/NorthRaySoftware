# Shared metadata

Single source of truth for the launcher and the website.

- `apps/` - one file per application. Drop a new file here to register an app; no launcher code changes needed.
- `brand/tokens.json` - colours, type and radii shared by every surface.
- `community/` - links used by NorthRay Launcher (download page, catalogue).
- `demo/` - demonstration content. Every file here has `"demo": true` and is only loaded when demonstration data is switched on.

Demo files use relative dates so they never go stale.

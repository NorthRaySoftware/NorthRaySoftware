# Shared metadata

Single source of truth for the launcher and the website.

- `apps/` - one file per application. Drop a new file here to register an app; no launcher code changes needed.
- `brand/tokens.json` - colours, type and radii shared by every surface.
- `community/` - links used by CJ Philip Community (download page, catalogue).
- `radio/schedule.json` - the real CJ Radio schedule. Only `confirmed`, `cancelled` and `postponed` programmes are shown to listeners. A series never publishes anything on its own.
- `demo/` - demonstration content. Every file here has `"demo": true` and is only loaded when demonstration data is switched on.

Times are written in UK wall-clock time (`startUk`, Europe/London) and resolved to UTC with GMT/BST rules. Demo files use relative dates so they never go stale.

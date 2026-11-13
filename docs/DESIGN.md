# Design language

The aim is software that looks like a small, careful team made it, not a template.

## Principles

- **Information first.** Each screen opens with what people need right now (on the Home screen, whether CJ Radio is on and what's coming up) and keeps secondary things quieter.
- **Rules, not cards.** Content sits in rows separated by thin rules, like a well-set page. Panels and boxes appear only when they mean something: the radio console, a notice, an action area.
- **Ornament with a reason.** The bookmark ribbon (books, and "you are here" in navigation) and the radio dial markings are the only decorative motifs. No gradients, glows or glass.
- **Plain words.** Buttons say what they do. Empty states say what's true ("Nothing has been confirmed yet") and what will change it. Unbuilt features say "Not built yet".
- **Never pretend.** Nothing shows as live, online, installed or downloadable unless it is. Demonstration content is always tagged DEMO and can be hidden in Settings.

## Tokens

Colours, type and radii live in `shared/metadata/brand/tokens.json`. The launcher and website both use them.

- Light theme: warm paper (`#F3EFE6`) and ink (`#1E1D1A`), ribbon claret (`#6A2E3F`).
- Dark theme: charcoal (`#18181A`) with warm off-white text and a lighter ribbon pink.
- The CJ Radio panel stays dark in both themes. It's the "console" and the only place a red on-air lamp appears.
- Type: Segoe UI Variable on Windows 11 (Segoe UI on Windows 10). Display weights for titles; small upper-case labels for section headings. Corner radius 3 to 4 px.

## Per-application identity

Apps share the type, spacing and controls but have their own accent and artwork:

- **CJ Radio**: dark, warm, tuning-dial motifs, accent `#B4442C`.
- **Writing & Publishing Studio**: its own identity: a quiet, paper-first workspace in deep ink blue (`#2D4A6E`) with a ribbon bookmark motif shared with the suite. Book-like typography for content, plain UI type for controls.
- **SkillSwap** (postponed): graph-paper and pencil-stroke motifs, accent `#2F6B5A`.

## Review checklist before calling a screen done

1. Would a person know what to do first?
2. Is anything shown as active, live or available that isn't?
3. Does it hold up at 860 px wide, at 150 % scaling, and in dark mode?
4. Can every action be reached with the keyboard?
5. Is any text there just to fill space?

# NorthRay Design Guidelines (current)

This summarises the design language as built, at suite 0.169.0. It supersedes the outdated parts of `docs/DESIGN.md` (see issue B2 in `ISSUES_AND_MISSING_INFO.md`). It was drawn from the code (`libs/cjui`, `shared/metadata/brand/tokens.json`), the architecture documents and the director's UI/UX answers of 23 November 2026.

## 1. Principles

1. **Information first.** Each screen opens with what people need right now. Secondary things are quieter, not hidden.
2. **Never pretend.** Nothing shows as active, live, installed or available unless it is. Unbuilt features say so; there are no placeholders that look like features. Demo content is marked DEMO.
3. **Plain words.** Buttons say what they do. Empty states give one line about what goes here, and one button.
4. **Calm.** One button style; one accent, used for what's selected or important; fewer, quieter notes; comfortable density.
5. **Undo instead of asking.** Ask only when an action can't be undone (permanent deletion, many changes at once with their own backup).
6. **Consistent everywhere.** The same controls, menus, dialogs and type levels in every app.

## 2. Look

**Night sky**
- Used for title areas, sidebars and banners: deep blue (`night.top` #0A1020 to `night.bottom` #121B33).
- Twinkling stars (still when Windows' animations are off), kept 5 DIPs clear of text.
- An aurora band in green, teal, blue and violet: #5BE3B4, #2FC6A0, #3A8FD8, #9B7BE0.
- Gradients and glows are drawn as solid bands and rings so they look the same everywhere.

**Pages:** cool neutrals.

| Theme | Paper | Surface | Ink | Accent |
|---|---|---|---|---|
| Light | #F3F5F9 | #FFFFFF | #0E1526 | #0F8F6E (deepened for contrast in 0.140) |
| Dark | #0D1220 | #141B2C | #E9EEF7 | #5BE3B4 |

**Writing Studio's warm variant** (0.68)
- Paper #F4F1EA, surface #FFFDF8, ink #1E2433, faint ink #5F687B. Dark paper #12141B.
- Gold kickers (#7F6129 light, #D6B677 dark).
- Serif headings (Georgia at title size).
- Cards with radius 10 and soft shadows.

**Type**
- Segoe UI Variable (Text and Display), with Segoe UI as the fallback.
- Four levels used consistently: title, heading, body, small. Small capitals for section labels.

**Radii:** controls 7–8, panels and cards 10–12.

**App accents**

| App | Accent |
|---|---|
| Writing Studio | #2D4A6E |
| Browser | #2FC6A0 |
| Files | #3A8FD8 |
| Capture | #D84A5A |

**Icons**
- Line icons drawn in code on a 16-unit grid (`cjui/icons.hpp`), so they follow the palette. **No AI-generated artwork. No imitations of official social logos.**

## 3. Controls and patterns

| Pattern | How it works |
|---|---|
| Buttons | `calmFace`: radius 7, a soft frame that lifts on hover, an icon from the label's words when there's room. One style everywhere. In Writing Studio a setting chooses icons only (words as tooltip), words only, or both |
| Menus | Themed, keyboard-driven (arrows, Enter, Esc), with icons in the gutter. Long lists (13+ items) get a type-to-search field and cap at 14 rows, scrolling by wheel and keys with arrow markers |
| Dialogs | `cjui::messageBox` with the aurora band, an icon, primary and secondary buttons and keyboard support. It always fits the screen; long text scrolls in a framed box with a "More below" note (0.93). `askText` is a one-line prompt |
| Toasts | A small rounded note near the bottom, replacing status bars, with Undo where it applies |
| Window frame | Each app draws its own top area with minimise, maximise and close. Snap layouts work on maximise |
| Tabs and tools | Pages have a row of tabs and one "tools ▾" menu (Writing Studio's pattern since 0.68) |
| Editing in place | Click a name or summary where it's shown and type. Enter or a click elsewhere keeps the change; Esc puts it back |

**Never:**
- menu bars
- scroll bars (none at all, not even faint ones)
- status bars
- stars crossing text

## 4. Motion

- Eased page changes, rising toasts, hover lifts, sliding tab underlines and rail pills.
- The stars' ambient animation runs about every 200 ms, only while the window is active.
- Respect reduced motion.

## 5. Accessibility checklist (before calling a screen done)

1. Would a person know what to do first?
2. Is anything shown as active, live or available that isn't?
3. Does it hold up at 860 px wide, at 150% scaling, and in dark mode?
4. Can every action be reached with the keyboard? Is there a visible focus ring?
5. Is text contrast at least WCAG AA, including faint text?
6. Is colour ever the only signal? It must not be; add an icon, word or shape.
7. Is any text there just to fill space?

**Known gap:** no UI Automation provider yet, so screen readers can't read drawn controls.

## 6. Brand and identity

- The name and developer line come from `shared/metadata/brand/identity.json`: "Developed by the NorthRay Software project".
- Contact: NorthRaySoftware@protonmail.com.
- Never invent people, staff or company history.
- Don't use the director's personal creative works or brands in NorthRay materials. Use generic examples.
- Screenshots come from the invented demo library (`studio_seed_demo`), never from anyone's real books.

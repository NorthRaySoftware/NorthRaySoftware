// The writing page's own zoom (Writing Studio 0.97), apart from the interface's. Pure functions, tested in test/zoom.test.mjs.
export const WIDTHS = { narrow: "36rem", normal: "44rem", wide: "56rem", fit: "calc(100% - 48px)" };
// 50% to 200%, the same steps as the interface's. It scales the text and the column together (not the writing bar), and
// "auto" widens the column on a wide window.
export const ZOOM_STEPS = [0.5, 0.67, 0.75, 0.8, 0.9, 1, 1.1, 1.25, 1.5, 1.75, 2];
export const clampZoom = (z) => Math.min(2, Math.max(0.5, Number.isFinite(+z) && +z > 0 ? +z : 1));
export function stepZoom(z, dir) {
  if (dir === 0) return 1;
  z = clampZoom(z);
  if (dir > 0) return ZOOM_STEPS.find((s) => s > z + 0.004) ?? 2;
  return [...ZOOM_STEPS].reverse().find((s) => s < z - 0.004) ?? 0.5;
}
export function pageWidth(width, zoom) {
  const z = clampZoom(zoom);
  if (width === "fit") return WIDTHS.fit;
  if (width === "auto") return `max(calc(44rem * ${z}), min(68vw, calc(64rem * ${z})))`;   // wider on a wide window
  return `calc(${WIDTHS[width] || WIDTHS.normal} * ${z})`;
}

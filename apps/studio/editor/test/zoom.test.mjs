import test from "node:test";
import assert from "node:assert/strict";
import { ZOOM_STEPS, clampZoom, stepZoom, pageWidth } from "../src/zoom.js";

test("page zoom steps up and down through the interface's steps, and Ctrl+0 is 100%", () => {
  assert.equal(stepZoom(1, 1), 1.1);
  assert.equal(stepZoom(1, -1), 0.9);
  assert.equal(stepZoom(1.25, 1), 1.5);
  assert.equal(stepZoom(2, 1), 2);       // stays at the top
  assert.equal(stepZoom(0.5, -1), 0.5);  // and the bottom
  assert.equal(stepZoom(1.3, -1), 1.25); // from between steps, to the next one down
  assert.equal(stepZoom(1.3, 0), 1);
  let z = 0.5, n = 0;
  while (z < 2 && n < 20) { z = stepZoom(z, 1); n++; }
  assert.equal(n, ZOOM_STEPS.length - 1);
});

test("page zoom is kept between 50% and 200%; nonsense is 100%", () => {
  assert.equal(clampZoom(5), 2);
  assert.equal(clampZoom(0.1), 0.5);
  assert.equal(clampZoom("x"), 1);
  assert.equal(clampZoom(undefined), 1);
  assert.equal(clampZoom(-1), 1);
});

test("the column scales with the zoom; Fit fills; Auto widens on a wide window", () => {
  assert.equal(pageWidth("normal", 1), "calc(44rem * 1)");
  assert.equal(pageWidth("wide", 1.5), "calc(56rem * 1.5)");
  assert.equal(pageWidth("fit", 2), "calc(100% - 48px)");
  assert.equal(pageWidth("auto", 1), "max(calc(44rem * 1), min(68vw, calc(64rem * 1)))");
  assert.equal(pageWidth("nonsense", 1), "calc(44rem * 1)");
});

# Roadmap / deferred items

Working notes for the fork. Newest first.

## Alpha-mask mode follow-ups (logged 2026-09-19)

- **Pure-glass tuning knobs** (evaluate, not yet decided):
  - Lower `AlphaMaskThreshold` (0.05–0.1) so any visible background gets full
    blur. Trade-off: edge antialiasing compresses because coverage saturates
    within a few percent alpha; very soft edges regain a slight stair.
  - `region` override per class (e.g. `org.kde.plasmashell`) to keep the old
    "blur wherever the region says" behaviour for a specific surface while
    everything else stays alpha-shaped.
- **v2 coverage estimator, opacity-independent AA**: normalise by the local
  fill alpha instead of a fixed threshold, e.g. `coverage = a / maxLocal`
  where `maxLocal` is the maximum alpha in a small kernel (or sampled a few
  pixels inward along the edge). Keeps full blur under translucent fills and
  full antialiasing at edges regardless of fill opacity. Cost: a few extra
  taps, only meaningful at edges.
- ~~D-Bus per-surface shape params~~ (done 2026-09-19): `setSurfaceShape` /
  `clearSurfaceShape` / `surfaceShapes` + `surfaceShapesReset` signal. The
  region-mining contract in `Window::getFinalBlurRegion()` stays as the
  fallback for `arrowCenterX`. Surfboard side still to be wired: publish on
  start and on `surfaceShapesReset`.
- ~~Plate flicker under alpha/inherit shape~~ (fixed 2026-09-19): KWin
  attaches a resized buffer to the surface texture lazily during its own item
  render, after effects draw, so every resize frame sampled a stale-size
  texture, failed the size guard and fell back to full geometric blur. The
  alpha pass now runs `Item::preprocess()` on the surface first (idempotent).
  Path changes remain visible in the journal as `[alpha-diag]` (info level);
  the alpha readback needs `kwin_effect_better_blur_dx.debug=true`.

# Surfboard integration: alpha-masked blur and D-Bus surface shapes

Better Blur DX (this fork) gained two things on 2026-09-19 that change how
surfboard should talk to it:

1. **Alpha-masked blur.** The blur behind a window can be shaped by the
   window's own alpha channel instead of a compositor-side SDF or the
   integer `wl_region` the Wayland protocol allows.
2. **D-Bus per-surface shapes.** Shape kind, radius, squircle exponent,
   pointed-arrow position and mask inset can be published per `app_id` at
   runtime, replacing the config-table edits and the two-rect region
   contract surfboard uses today.

This document explains both from surfboard's point of view and lists the
accommodations to make. Effect-side details: `configuration.md` (sections
"Shape blur by window alpha" and "D-Bus") and `CHANGELOG.md`.

## 1. Why: what the protocol cannot do

`ext_background_effect_v1` (and the legacy `org_kde_kwin_blur`) carry one
thing: `set_blur_region(wl_region)`. A `wl_region` is a union of integer
rectangles. No paths, no radii, no alpha. A rounded or squircle corner can
only be approximated as a stair of 1 px rectangles, which is why blur
corners looked jagged, and why this fork grew per-class SDF masks
(squircle, flat-bottom squircle, pointed) keyed on surfboard's app_ids.

Those masks needed parameters the protocol has no room for, so surfboard
resorted to:

- writing `PlateMaskInset`, `SquircleExponent`, `PointedTooltip*` and
  `MenuRadius` into `kwinrc` and asking the effect to reconfigure;
- `previewDockSurfaces(...)` over D-Bus for live previews of those globals;
- encoding the tooltip / thumbnail arrow position as a second, narrow
  rectangle in the blur region (`Window::getFinalBlurRegion()` mines it);
- a "body + one bottom rect" region contract so `surfboard-menu` is
  recognised as a pointed menu.

Both new features remove most of that.

## 2. Alpha-masked blur

### What the effect does

With **General → Corners → "Shape blur by window alpha"** on
(`AlphaMask=true`), an eligible window's blur is composited like this:

- the client's blur region is widened to its bounding rectangle;
- the cached blur is multiplied per pixel by
  `clamp(surfaceAlpha / AlphaMaskThreshold, 0, 1)`, where `surfaceAlpha`
  is the maximum alpha over the root surface and up to three subsurfaces
  (GTK4 graphics offload, video);
- everything else (blur strength, noise, colour matrix, refraction) is
  unchanged.

The window's own antialiasing becomes the blur's antialiasing. Any shape
works: rounded, squircle, arrow, animated morph, holes. `AlphaMaskThreshold`
(default 0.5, "Full-blur alpha" in the KCM) is the surface alpha at which
blur reaches 100 %.

Eligible by default: undecorated windows that requested blur themselves and
have no explicit `squircle` / `pointed` shape and are not menus or
notification toasts (KWin notification / critical-notification role; their
in-surface shadow would be read as fill). Explicit
opt-in / opt-out per class via the shape `alpha` / `region`.

### Consequences surfboard must know

- **Blur follows what you draw.** A fully transparent surface gets no
  blur. If the plate background is set to opacity 0, alpha mode yields no
  plate blur at all; that is by design. The old "pure glass" look (blur
  with no tint) needs either a small non-zero fill, a lower threshold, or
  the SDF path.
- **Fill alpha does not change blur strength** in the default *silhouette*
  mode: anything above the floor (0.02) gets full blur, edges keep the
  window's antialiasing. The older *threshold* mode (coverage = alpha /
  threshold, 0.30 fill → 60 % blur) is still selectable. Check
  `alpha-mask-mode=` in `capabilities()`.
- **Silhouette's edge heuristic reads one texel around each pixel.** An
  antialiased edge wider than that (rotated or resampled client content,
  e.g. a tilted label rendered through a transform) is stepped instead of
  ramped. For such a surface publish `kind=alpha-threshold` with its own
  `T` (section 3): that class gets threshold coverage
  `clamp(alpha / T, 0, 1)`, which reproduces the client's alpha ramp
  exactly at any width, while every other alpha-masked window keeps the
  global mode. Blur strength then follows fill alpha for that class only,
  so draw the fill at or above `T`. Check for the `alpha-threshold`
  capability token first.
- **In-surface soft shadows are blurred to their floor contour** in
  silhouette mode. Use compositor shadows (KWin shadow protocol) for fan
  labels and grid panels, or keep shadow alpha under the floor.
- **Only the client region's bounding box is blurred.** Keep requesting
  a blur region (any rectangle covering the shape is fine); without a
  request the window is not eligible unless it carries an `alpha`
  override.
- **Surfaces the effect already shapes with an SDF are not touched.**
  `surfboard-plate` (squircle override), `surfboard-tooltip` (pointed by
  class), `surfboard-menu` (menu mask) keep their current look until you
  change their shape kind.
- **Subsurfaces are sampled but clipped to the root surface's blur
  region.** Content that extends beyond the root surface gets no blur.
- Requires Plasma 6.7. On older KWin the toggle is inert and windows stay
  on the geometric path.

### Mask registration at fractional scales (fixed 2026-10-21)

**Symptom.** At scale 1.5 a one-device-pixel strip of blur peeked past
alpha-masked surfaces: always along the left or top edge of the stacks
Grid panel, at the left ends of some Fan pills, and blur went missing
along the opposite edge. Which edges showed it depended on where the
window opened.

**Cause.** KWin 6.7 snaps what it draws: `ItemRendererOpenGL::
createRenderNode` translates every item by its own position rounded to
device pixels, and `RenderGeometry` rounds each quad vertex
(`VertexSnappingMode::Round`). The alpha pass placed the surface texture
at the *unrounded* device position (the old code argued sub-pixel
placement keeps the client's antialiasing where the client put it). A
window at an odd logical x or y lands on half a device pixel at 1.5:
KWin drew it at e.g. x = 1172 while the mask was read for x = 1171.5, so
every sample fell between two texels. The bilinear read spread the
edge's alpha one pixel outwards (ring alpha 0.69 read as 0.345 on the
transparent pixel beside it) and pulled it in on the far edge. In
threshold mode coverage is `alpha / T` with T = 0.30 (Grid) or 0.32
(Fan), so 0.345 saturated: full blur on a pixel the client left fully
transparent. Silhouette mode suffers the same misplacement, only less
visibly. Windows on whole device pixels were never affected.

**Fix.** `drawAlphaMasked` computes the surface box the way KWin places
the surface: the window item's position rounded to device pixels, plus
each item down to the surface item rounded on its own, plus the window
transform (`WindowPaintData` translation and scale) applied on top, with
the size `round(logical size * scale)`. Mask texels now coincide with the
pixels KWin draws, so edge pixels read the client's own alpha.

**How it was measured.** In the headless KWin rig
(`~/dev/surfboard-grid-rig`, `run-bdgrid.sh` + `session-bdgrid.sh`) over
a black/white noise wallpaper, solving `D = C + (1 − a)(c·B + (1 − c)·S)`
per pixel for the coverage `c`, and counting pixels where `c` exceeds
`min(1, a / T)`. Before: 2554 such pixels on a Grid at odd x and y, 885 to
3182 on the Fan depending on placement, all on window-parity edges.
After: 0 at every placement, against a control built from the same
source without the change. When checking a surface by eye, note that a
screenshot taken under KWin's zoom effect magnifies everything; measure
a known feature first.

### Recommendation per surfboard surface

| app_id | Today | Suggested | Why |
| --- | --- | --- | --- |
| `surfboard-plate` | squircle SDF (+ flat bottom, inset, exponent) | keep SDF, publish params over D-Bus | refraction and the exact superellipse are SDF features; alpha mode would follow the 0.3 fill and drop to 60 % |
| `surfboard-tooltip` | pointed SDF + region-mined arrow | keep SDF, publish `arrowCenterX` over D-Bus | arrow geometry (height, shoulder, tip) is analytic; alpha would work but the SDF already matches the drawn shape |
| `surfboard-thumbnailer` | pointed via override + region-mined arrow | same as tooltip | |
| `surfboard-menu` | pointed menu via two-rect contract | `kind=pointed` + `radius` over D-Bus, keep the contract until every install has this build | removes class + geometry sniffing |
| `surfboard-settings-surface` | force-blurred, radius 20 | try `alpha` | CSD-style window, corners follow the drawn shape |
| `surfboard-dialog` / `-dim` | force-blurred | `alpha` for the dialog, `region` for the dim layer | dim layer is a plain rectangle |
| `surfboard-folder-grid` (new) | — | `pointed` + `setSurfaceArrow` (own arrow size, `edge` facing the dock) | same analytic body/arrow as the tooltip, per-surface geometry |
| `surfboard-folder-fan` (new) | — | `alpha` with a tight blur region | arbitrary, animated silhouette; nothing to keep in sync |
| tilted / resampled alpha content (e.g. a `quickshell` label layer) | — | `alpha-threshold` with `radius=T` | wide antialiasing ramps step under silhouette's one-texel heuristic; threshold coverage follows the ramp exactly |

Whatever you pick, the shape is now a one-line D-Bus call away, so it is
cheap to experiment.

## 3. D-Bus contract

Service `org.kde.KWin`, object `/org/kde/KWin/Effects/BetterBlurDX`,
interface `org.kde.KWin.Effects.BetterBlurDX`, session bus. Existing
`previewDockSurfaces` stays as is.

```
bool        setSurfaceShape(string appId, string kind,
                            double radius, double exponent,
                            double arrowCenterX, double inset)
bool        clearSurfaceShape(string appId)
array<string> surfaceShapes()
array<string> capabilities()
signal      surfaceShapesReset()
```

`capabilities()` returns tokens: `api=1`, `surface-shape`, `surface-arrow`,
`remnant-alpha`, and on a Plasma 6.7 Wayland build `alpha-mask` and
`alpha-threshold` (kind `alpha-threshold` is understood), plus
`alpha-mask-enabled`, `alpha-mask-threshold=<v>`,
`alpha-mask-mode=threshold|silhouette` and `alpha-mask-floor=<v>` when the
global toggle is on. Use it at startup and after `surfaceShapesReset` to
decide between `kind=alpha` / `alpha-threshold` and a geometric/plain-region
fallback; a successful
`setSurfaceShape` alone does not prove alpha masking is active (the
`[alpha-diag]` journal lines do, per window).

### Arguments

| arg | meaning | inherit sentinel | clamp |
| --- | --- | --- | --- |
| `appId` | exact resource class (Wayland `app_id`) | — | must be non-empty |
| `kind` | `inherit`/`""`, `rounded`, `squircle`, `squircle-flat`, `pointed`, `alpha`, `alpha-threshold`, `region` | `inherit` or `""` | unknown → `false` |
| `radius` | corner radius, logical px; for `alpha-threshold` the saturation alpha `T` | `< 0` | 0 … 512; `alpha-threshold`: 0.01 … 1.0 |
| `exponent` | superellipse exponent for squircle bodies and pointed bodies; 2 = rounded rectangle | `<= 0` | 2 … 8 |
| `arrowCenterX` | pointed-mask arrow tip centre, **frame-local logical x** | `< 0` or NaN | 0 … 65536 |
| `inset` | mask inset, logical px (squircle plate, pointed body) | `< 0` | 0 … 64 |

Semantics:

- One shape per `appId`; a second call replaces the first entirely
  (fields you leave at the sentinel go back to inherit, they are not
  merged with the previous call).
- Inherited fields come from the config override that matches the class
  (exact, regex or `$blank`), then from the globals (`SquircleExponent`,
  `PlateMaskInset` for the plate, `PointedTooltipInset`, region-mined
  arrow, …). Published values win over both.
- `kind=rounded` forces a plain rounded rectangle and clears any
  squircle/pointed/alpha/region flag the config override had.
- `kind=alpha` opts the class into the alpha-mask path and follows the
  global `AlphaMaskMode` / `AlphaMaskThreshold` / `AlphaMaskFloor`.
- `kind=alpha-threshold` opts the class in the same way but uses threshold
  coverage `clamp(alpha / T, 0, 1)` for that class regardless of the global
  mode. `T` travels in `radius` and is clamped to 0.01 … 1.0 (`< 0` or NaN
  inherits the global `AlphaMaskThreshold`); `exponent`, `arrowCenterX` and
  `inset` are ignored for this kind (send 0, -1, 0). The corner radius is
  not touched (it keeps inheriting, and only matters on the geometric
  fallback). `surfaceShapes()` prints `T` in the radius slot. Requires the
  `alpha-threshold` capability token and, like `alpha`, the global
  `AlphaMask` toggle (`alpha-mask-enabled`) to take effect.
- Applies immediately to every mapped window of that class and to
  windows mapped later. Survives a config reconfigure (re-merged on top of
  the new table).
- `radius` also feeds `pointedMaskBodyRadius` for pointed shapes, so a
  pointed menu published with `radius` no longer depends on `MenuRadius`.
- `surfaceShapes()` returns `"<appId> <kind> <radius> <exponent>
  <arrowCenterX> <inset>"` with `inherit` for unset fields; for debugging.
- Returns `false` and logs a warning for an empty `appId`, an unknown
  `kind`, or when the effect is not initialised; `clearSurfaceShape`
  returns `false` if nothing was published for that class.

### Coordinate conventions

`arrowCenterX` is **frame-local logical** x: measured from the left edge of
the window's frame geometry, in logical (unscaled) pixels. For a
layer-shell or xdg surface without server-side decoration the frame is the
surface, so this is simply surface-local x. It is what the region contract
produced, i.e. `arrowRect.horizontalCenter() + contentsRect.left()`.

Not device pixels, not relative to the body centre. The effect clamps it
so the arrow stays on the body, the same way surfboard's own drawing does.

`radius`, `exponent` and `inset` are the same values you write to `kwinrc`
today; nothing changes in their meaning, only in delivery.

### Arrow geometry and edge: `setSurfaceArrow`

```
bool setSurfaceArrow(string appId, string edge,
                     double height, double halfWidth,
                     double shoulder, double tipRadius, double feather)
```

| arg | meaning | inherit sentinel | clamp |
| --- | --- | --- | --- |
| `edge` | `bottom`, `top`, `left`, `right` | `""` / `inherit` = bottom | unknown → `false` |
| `height` | arrow height, logical px | `< 0` | 0 … 64 |
| `halfWidth` | half the arrow base, logical px | `< 0` | 0.5 … 64 |
| `shoulder` | fillet between body and arrow flank, logical px | `< 0` | 0 … 64 |
| `tipRadius` | tip rounding, logical px | `< 0` | 0 … 32 |
| `feather` | antialiasing width multiplier | `< 0` | 0.25 … 4 |

- Independent of `setSurfaceShape`: each call replaces only its own group
  of fields. Calling `setSurfaceArrow` first creates the class entry with
  every shape field inheriting. `clearSurfaceShape` removes both.
- `edge` rotates the whole pointed mask. On `left`/`right`, the
  `arrowCenterX` you publish with `setSurfaceShape` is read as frame-local
  **y** (position along that edge). Side docks: publish the edge that faces
  the dock.
- Inherited values are the global `PointedTooltip*` keys, which remain the
  defaults for every pointed surface.

Only the plate blur-radius preview stays purely global
(`previewDockSurfaces`).

### Blur fades with `wp_alpha_modifier`

Any window that commits a `wp_alpha_modifier` factor below 1 couples its
blur opacity to that factor, exactly as `surfboard-tooltip` did by class.
No publication is needed for this: a published shape or arrow couples too,
but a plain `region` override or no override at all works the same the
moment the client sends a multiplier. Fade a folder grid or fan with the
alpha modifier and the blur fades in the same frame. Windows that never use
the protocol are unaffected.

KWin keeps a closing window alive for its close animation after the Wayland
surface is destroyed. For these windows the effect keeps the last committed
alpha factor for that retained window (`remnant-alpha` capability), so a
surface that faded to zero does not regain full blur for its remnant frames.
Surfboard's client-side barrier (clear the blur region, `wl_display_sync`,
then hide) is still a good idea; the two compose.

## 4. Lifetime, startup and reload

Published shapes are **in-memory**. They disappear when:

- the effect is unloaded/reloaded (KCM apply of some settings, manual
  `unloadEffect`/`loadEffect`, compositor restart);
- KWin itself restarts.

Handle both:

1. **On surfboard start**, call `capabilities()` and publish all shapes.
   The object may not exist
   yet (KWin still starting, effect disabled): the call fails with
   `org.freedesktop.DBus.Error.UnknownObject` / `ServiceUnknown`. Treat a
   failure as "retry later", e.g. exponential backoff up to ~10 s, then
   give up quietly; the effect will work with the config table as before.
2. **Subscribe to `surfaceShapesReset`** on that object and republish when
   it fires. The signal is emitted after the effect registers its object,
   so a publish from the handler always lands.
3. **Watch `NameOwnerChanged` for `org.kde.KWin`** (or
   `g_bus_watch_name`) and republish when a new owner appears; this covers
   compositor restarts, where no reset signal can be delivered.
4. Republish whenever surfboard's own settings for a surface change
   (radius, exponent, inset) instead of editing `kwinrc` and calling
   reconfigure. The `kwinrc` write can stay for persistence across effect
   restarts if you like, but it is no longer needed for the live update.
5. On surfboard shutdown, `clearSurfaceShape` for each class is polite but
   optional; the next KWin/effect restart drops them anyway.

With GDBus this is one proxy plus one signal handler; a `gdbus` shell
equivalent for reference:

```sh
gdbus call --session --dest org.kde.KWin \
  --object-path /org/kde/KWin/Effects/BetterBlurDX \
  --method org.kde.KWin.Effects.BetterBlurDX.setSurfaceShape \
  surfboard-plate squircle-flat 32 3.1 -1 1.0
gdbus monitor --session --dest org.kde.KWin \
  --object-path /org/kde/KWin/Effects/BetterBlurDX   # shows surfaceShapesReset
```

## 5. Migration checklist for surfboard

Do these in order; each step is independently shippable.

- [ ] Add a `BetterBlurShapes` helper: proxy to the object, `publishAll()`,
      retry-on-start, `surfaceShapesReset` handler, `NameOwnerChanged`
      handler.
- [ ] Publish `surfboard-plate`: `kind=squircle-flat` (or `squircle` when
      not docked to an edge), `radius`, `exponent`, `inset`. This replaces
      the `PlateMaskInset` / `SquircleExponent` kwinrc writes for live
      changes.
- [ ] Publish `surfboard-tooltip` with `kind=pointed`, `radius`,
      `exponent` and `arrowCenterX` on every layout that moves the arrow.
      Once every target install has this build, drop the second region
      rectangle; keep the body rectangle as the blur region.
- [ ] Same for `surfboard-thumbnailer`.
- [ ] Publish `surfboard-menu` with `kind=pointed`, `radius` (today's
      `MenuRadius`) and `arrowCenterX`; later drop the "body + bottom
      rect" region shape and send a plain body rectangle.
- [ ] Folder grid: `setSurfaceShape … pointed` + `setSurfaceArrow` with the
      grid's own arrow size and the edge facing the dock; publish
      `arrowCenterX` (or y for side docks) per opening.
- [ ] Folder fan: request a blur region equal to the fan's bounding box,
      publish `kind=alpha`; fade with `wp_alpha_modifier`.
- [ ] Try `kind=alpha` on `surfboard-settings-surface` and
      `surfboard-dialog` with `AlphaMask` on; check corners and edge
      quality at fractional scale.
- [ ] Settings UI: the live preview for plate/tooltip params can call
      `setSurfaceShape` directly; `previewDockSurfaces` remains for the
      arrow geometry globals.
- [ ] Remove the `surfboard-*` class special-cases you no longer rely on
      from your own docs; the effect keeps them as fallbacks, so old
      surfboard builds keep working against new effect builds.

Compatibility matrix: new effect + old surfboard = unchanged behaviour
(region contract and config table still honoured). Old effect + new
surfboard = the D-Bus calls fail (`UnknownMethod`), so guard the publish
with the retry logic above and keep the region contract until you can
require this effect build.

## 6. Verifying

```sh
# what is published right now
qdbus6 org.kde.KWin /org/kde/KWin/Effects/BetterBlurDX surfaceShapes
# threshold coverage (T = 0.32) for one class while the global mode stays silhouette
qdbus6 org.kde.KWin /org/kde/KWin/Effects/BetterBlurDX setSurfaceShape quickshell alpha-threshold 0.32 0 -1 0

# effect-side log of every publish / clear, and of alpha-mask path changes
journalctl --user -f -o cat | grep -E "Surface shape|alpha-diag"

# per-window alpha readback and origin changes (verbose)
# ~/.config/systemd/user/plasma-kwin_wayland.service.d/bbdx_debug.conf
#   [Service]
#   Environment=QT_LOGGING_RULES=kwin_effect_better_blur_dx*.debug=true
```

`[alpha-diag] <class> -> alpha path` confirms a window is on the alpha
mask; `-> geometric fallback: <reason>` names why it is not (opaque
buffer, YUV, no texture yet, transformed buffer, mirror mismatch).

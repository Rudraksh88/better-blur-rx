# DEV
Things not in any tagged release yet:

### Features
- **Alpha-masked blur** (General → Corners → "Shape blur by window alpha",
  off by default). Undecorated windows that request blur themselves (CSD
  apps, layer-shell panels, docks) have their blur shaped by their own alpha
  channel at the final composite instead of the integer `wl_region` the
  protocol allows, so rounded, squircle or arbitrary corners inherit the
  client's own antialiasing with no per-class SDF. The client's region is
  widened to its bounding rect for these windows; the alpha pass carves the
  silhouette. "Full-blur alpha" sets the window alpha at which coverage
  saturates (default 0.5). Explicit squircle/pointed overrides and the menu
  mask keep priority. New override shapes: `alpha` opts a window in regardless
  of scope (e.g. force-blurred CSD apps), `region` opts it out. Falls back to
  the geometric path when the surface texture is unusable (no texture yet,
  YUV, rotated buffer, opaque format). Samples the root surface and up to
  three subsurfaces (max alpha), so GTK4 graphics offload, which toggles a
  widget's content between the root and a subsurface frame by frame, no
  longer flickers the blur. The surface texture is brought up to date before
  sampling (KWin otherwise attaches a resized buffer only during its own item
  render, after effects), so window resize animations keep the alpha mask
  instead of dropping to full geometric blur for a frame. Composite path
  changes per window are logged at info level as `[alpha-diag]`.
  Any window that commits a `wp_alpha_modifier` multiplier below 1 now has
  its blur opacity follow that multiplier, not only published shapes and the
  dock tooltip; windows that never use the protocol are untouched.
  Notification and critical-notification windows stay on the geometric path
  by default (their in-surface drop shadow would be read as fill and blur
  the region's bounding box); an `alpha` override opts them in.
  Requires Plasma 6.7.
- **D-Bus per-surface shapes.** `setSurfaceShape(appId, kind, radius,
  exponent, arrowCenterX, inset)`, `clearSurfaceShape(appId)` and
  `surfaceShapes()` on `/org/kde/KWin/Effects/BetterBlurDX` let a client
  publish, at runtime and per resource class, what the override table and
  the region-mining contract used to carry: shape kind, radius, squircle
  exponent, pointed-arrow position and mask inset. Published values sit on
  top of the matching config override; unset fields inherit. The
  `surfaceShapesReset` signal tells clients to publish again after an
  effect reload. `setSurfaceArrow(appId, edge, height, halfWidth,
  shoulder, tipRadius, feather)` adds per-surface pointed-arrow geometry
  and lets the arrow sit on any edge (side docks); windows with a published
  shape couple their blur opacity to `wp_alpha_modifier` like the dock
  tooltip. See docs/configuration.md and docs/surfboard-integration.md.
- **Alpha mask "silhouette" mode** (new default, KCM General → Corners →
  "Alpha mask mode"). Coverage no longer follows the fill alpha: any fill
  above "Ignore alpha below" (default 0.02) gets full blur, and edges against
  transparency use the window's own antialiasing (alpha over the one-texel
  neighbourhood maximum). A 0.05 fill now blurs exactly like an opaque one.
  The previous behaviour (coverage = alpha / "Full-blur alpha") remains as
  "Threshold" mode. `capabilities()` gains `alpha-mask-mode=` and
  `alpha-mask-floor=`.
- **Per-surface alpha coverage mode.** New D-Bus shape kind
  `alpha-threshold`: opts a class into the alpha-mask path like `alpha`, but
  composites it with threshold coverage (`clamp(alpha / T, 0, 1)`) while
  every other window keeps the global mode. `T` is passed in the `radius`
  argument (clamped to 0.01–1.0, `< 0` inherits "Full-blur alpha"); the
  other numeric arguments are ignored. Meant for surfaces whose antialiased
  edges are wider than silhouette mode's one-texel heuristic (rotated or
  resampled content). The mode and threshold uniforms are now resolved per
  window at the composite. `capabilities()` gains `alpha-threshold`.
- `capabilities()` on the same D-Bus object reports what the running build
  and configuration support (`api=1`, `surface-shape`, `surface-arrow`,
  `remnant-alpha`, `alpha-mask`, `alpha-mask-enabled`,
  `alpha-mask-threshold=<v>`), so a client can tell "the API exists" from
  "alpha masking is on" without reading kwinrc.
- The override editor's "Squircle corner shape" checkbox is now a "Blur
  shape" combo covering all shape kinds; `pointed` and `squircle-flat` rules
  no longer get dropped when the KCM rewrites the rule list.
- Add a per-window squircle shape override with an antialiased superellipse
  mask at the final cached-blur composite stage. Undecorated squircle windows
  use full coarse bounds so their configured radius remains authoritative
  instead of being clipped by an integer client blur region.

- Published shapes can target a class's dock-type windows only: a shape
  published as `<class>@dock` applies to dock windows of `<class>` ahead of
  a plain `<class>` shape, and inherits that class's config override. Two
  surfaces of one executable can now carry different shapes (surfboard's
  QML dock and its stacks are both `quickshell`). `capabilities()` gains
  `surface-key-dock`. Details in `docs/surfboard-integration.md`,
  "Dock-type keys".

### Bug Fixes
- Alpha-masked blur no longer peeks one device pixel past a window placed on
  half a device pixel (an odd logical position at scale 1.5). The mask used
  the surface's unrounded device position while KWin draws every item at its
  position rounded to device pixels, so the mask sat half a texel off the
  drawn surface and the bilinear read bled the edge's alpha outwards; with
  threshold coverage that bleed became full blur. The mask is now placed
  exactly where KWin draws the surface. Details in
  `docs/surfboard-integration.md`, "Mask registration at fractional scales".
- Published `alpha-threshold` shapes no longer get a sub-pixel blur halo
  outside their antialiased edge. `alpha / T` over-covered rim pixels by the
  outer layer's alpha over `T` (about 1.5× on the stacks' labels, full blur
  from 64 % covered); rim pixels now get `alpha / max(T, 5×5 maximum)`, the
  share of the pixel the client covered, while fill and soft shadows keep
  `alpha / T`. Global alpha-mask modes are unchanged. `capabilities()` gains
  `alpha-threshold-rim`. Details in `docs/surfboard-integration.md`, "Exact
  rim coverage for `alpha-threshold`".
- `alpha-threshold` rim pixels get the blur that composites like the
  client's own antialiasing, `(f − a) / (1 − a)` for a pixel the client
  covers by share f at alpha a, instead of the geometric f, which put up to
  three times too much blur behind a translucent outline's edge pixel (a
  faint line along the dock plate's darker sides). Soft shadows and
  interiors are unchanged. Details in `docs/surfboard-integration.md`,
  "The last faint line".
- Published `alpha-threshold` shapes honour `inset` as the place their
  paint starts: coverage is capped by each pixel's overlap with the surface
  rect inset by it (sides flush with the output edge excepted). The rim
  estimate divides by the brightest nearby alpha, which never reaches a
  sub-pixel outer ring's full alpha on a straight edge; the dock plate's
  0.64 px Glass ring got 0.79 blur on its sides where it covers 0.50. The
  rim test also reads past the buffer edge as transparent instead of the
  clamped edge texel, for surfaces painted up to their border.
  `capabilities()` gains `alpha-threshold-inset`; inset 0 changes nothing.
  Details in `docs/surfboard-integration.md`, "The dock plate on its own
  alpha".
- Loading the effect while a force-blurred window was open crashed KWin:
  the window manager's constructor adds existing windows, and a
  force-blurred one called back into `BlurEffect::updateBlurRegion()` before
  the manager pointer was assigned (null dereference in `findWindow`).
- Windows with a published D-Bus shape (and `surfboard-tooltip`) keep the
  last `wp_alpha_modifier` factor their surface committed while KWin retains
  them for the close animation. Previously the opacity getter fell through
  to full opacity once the Wayland surface was gone, so a popup that had
  faded to zero regained full blur for its remnant frames. The coupling is
  latched per window, so a client clearing its published shape while KWin
  still animates the remnant cannot re-enable blur; `setSurfaceShape` /
  `clearSurfaceShape` also leave surfaceless (closing) windows untouched.
- Apply regular rounded-corner masks during the final cache composite, matching
  the squircle path. The old cache-stage pass sampled from and rendered into the
  same texture, an undefined OpenGL feedback loop that could produce flickering
  rectangular or colorful pixel blocks during surface damage.

# 2.5.1

### Bug Fixes:
- Adapt to KWin shader load changes [KWin MR](https://invent.kde.org/plasma/kwin/-/merge_requests/9053)
  On Plasma 6.7+ we now explicitly load the `_core` version of shaders

# 2.5.0

### Features
- **Plasma 6.7 support**
- **BlurCache rework**
  The cache is now only used to rate-limit frequent repaints
  (currently hard coded ~30fps).
  After a bunch of experiments the conclusion was made that
  comparing textures to decide whether to use the cache or not
  is not worth it. At best it's just very slightly faster than blurring
  unconditionally and at worst you have to pay for both the compare
  *and* the re-blur on difference (which could also just occur randomly
  because of various rounding issues).

### Internal
- Don't invalidate cache on opacity change (this was left over from
  when opacity was still part of the cached texture)
- There is now a unified `kwin_compat.hpp` header which automatically
  includes `kwin_version.hpp` and all appropriate `kwin_compat_<version>.hpp` headers
  depending on the build environment.

# 2.4.1

Version bump to fix version string - no actual changes

# 2.4.0

### Features:
- **Blur Caching** to (hopefully) improve performance of the effect.
  This is my 3rd attempt at "making the effect less GPU hungry"
  and changes a good chunk of the rendering pipeline.
  At least on my machine and testing VM it does look quite promising
  and I haven't seen any issues reported since the introduction in `main`
- **Higher Noise Strength Limits** in case you wanted to increase those more without
  digging through `kwinrc`. The limit was increased from 14 to 50.
- **Spinboxes for Blur Strength and Noise Strength** if you prefer numbers over sliders - and to be consistent with the other options.

### Bug Fixes:
- Fixed `maximizedState` detection with "slim" panels that don't cover a full screen edge
- Fixed incomplete repaints e.g. when a video is playing behind a blurred surface (needs KWin 6.6.4+)

### Internal
- Cleaned up a bunch of dead code and shaders
- Assume kwin-x11 has API version 6.5.0 (it hasn't really seen any updates since then)
- `WindowManager` now talks directly to the `BlurEffect` without going through the Qt slot loop

# 2.3.0

### Features:
- New rounded corners pass to stop rectangular noise texture from "leaking"
- Debug logging for (toggled with `QT_LOGGING_RULES=kwin_effect_better_blur_dx.*.debug=true`):
  - Added windows
  - Removed windows
  - `blurOrigin` changes
  - `maximizedState` changes

### Bug Fixes:
- More pedantic `isPlasmaSurface` check. We now need to be *very sure* something
  is a special Plasma surface (to the degree of matching window classes) and else
  assume it isn't; should be the better trade-off because most bugs I keep seeing
  boil down to "this is a false-positive in `isPlasmaSurface`" and only rarely
  "this should be treated as a Plasma surface"
- Only take "opacity affects blur" path if opacity changed at during a window's lifetime.
- Explicitly calculate `maximizedState` once on `BBDX::Window` creation and
  track our own `isFullscreen`/`isMinimized`. Fixes rounding of windows that spawn
  fullscreen/maximized and don't change their geometry after (like the Plasma logout greeter).

### Internal:
- Most `shouldForceBlur` logic is now attached to `BBDX::Window` (instead of `BBDX::WindowManager`)
  which saves some `std::unordered_map` lookups.

# 2.2.0

### Features:
- Port to Plasma 6.6 blur
- Support Plasma 6.6
- Re-implement refraction as an alternative to the default contrast pass

### Bug Fixes:
- Fixed falsely overwriting blur regions with empty force-blur regions resulting e.g. in decorations losing their blur.

# 2.1.0

### Features:
- Match window classes with regular expressions
- Improved "blur while transformed" behaviour - and thus compatibility with Wobbly Windows.  
  Before the behaviour of BBDX in conjuction with Wobbly Windows was somewhat "undefined" (blur randomly disappearing, flickering, blur region "leaking" behind the window, ...).  
  Now we explicitly disable blur on window move/resize, let it transform like it wants, then explicitly reapply our blur.  
  It's not perfect but it's consistent and predictable. (And there even is a very cool fade when transitioning between blurred ↔ non-blurred states 🚀)

### Bug Fixes:
- Fix not respecting requested blur by X11 clients on the Wayland build. The X11 property parsing logic was accidentally made exclusive to the X11 build.
- Fix corner radius with "Round bottom corners of windows with no borders" (Breeze decoration settings) and "Blur decorations as well" both enabled.

### Internal:
- Introduce `WindowManager` and `Window` classes to track all relevant window info, perform matching, etc.

# 2.0.0
The first release of Better Blur DX 🎉

> [!IMPORTANT]
> If coming from the original Better Blur: This release contains **breaking changes**.

### Features:
- Port to the Plasma 6.5 blur
- ***BREAKING:*** Changed config group name - If coming from the original Better Blur you'll have to reconfigure
- ***BREAKING:*** Removed refraction - This needs a major rewrite in order to work with the new contrast pass of the Plasma 6.5 blur.
  If there is a lot of interest it might come back some day but for now it's gone to ease maintenance.
- ***BREAKING:*** Removed static blur - This used a bunch of hacks and IMHO isn't worth the maintenance effort.
  It essentially duplicated the *entire effect pipeline* for IMO very little gain.
- Contrast pass parameters can be configured via sliders or spinboxes
- Allow forcing contrast pass parameters - E.g. the Plasma panel provides its own values which we respect by default.
  This allows users to override those values if the window-provided values look "off"
- Auto detect "window opacity affects blur" - The option was removed and we now detect if a window's blur should be
  affected by its opacity or not based on some heuristics.

### Bug fixes:
- Fix blurring of windows with opacity < 1.0 (from KWin rules)
- Fix artifacts around screen edges in the new contrast pass

### Misc:
- Rename to "Better Blur DX"

# <= 1.5.0
Older version are from the original Better Blur - we just kept the version going from here.

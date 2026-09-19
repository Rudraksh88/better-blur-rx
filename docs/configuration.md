# General

### Blur Strength

Like the default KWin blur setting.
Adjusts how "sharp" the blurred background is.

### Noise Strength

Like the default KWin blur setting.
Adjusts how strong the noise applied to the blurred background is.
(Used to hide banding artifacts in smooth gradients.)

### Brightness

Part of the contrast pass.
Adjusts how bright the blurred background is.

### Saturation

Part of the contrast pass.
Adjusts how saturated the blurred background is.

### Contrast

Part of the contrast pass.
Adjusts how strong the contrast of the blurred background is.

### Force Contrast Parameters

By default Better Blur DX respects when windows provide their
own contrast pass parameters (brightness, saturation, contrast).

E.g. [Plasma surfaces provide their own values](https://invent.kde.org/plasma/libplasma/-/blob/v6.5.3/src/desktoptheme/breeze/plasmarc)
which may or may not look good to you.

To force your settings to be respected for all windows enable this.

### Corner Radius

Round the corners of the blurred background by this amount.
This is mainly useful to work around the infamous "korners" bug
(square blur region leaking out behind rounded windows).

> [!NOTE]
> This setting is ignored for windows that provide their own blur region (e.g. Plasma surfaces).
> We expect these to be set correctly by the window (and usually they are).

### Shape blur by window alpha
Off by default. Requires Plasma 6.7.

Undecorated windows that request blur themselves (CSD applications,
layer-shell panels, docks) have their blur shaped by their own alpha channel
at the final composite instead of the integer `wl_region` the Wayland protocol
allows. Rounded, squircle or arbitrary corners follow the antialiasing the
client drew, with no per-class shape rule. The requested region is widened to
its bounding rectangle for these windows and the alpha pass carves the exact
silhouette, so region holes are not preserved; use a `region` override to keep
a client's raw region.

Explicit `squircle` / `pointed` overrides and the menu mask keep priority.
Force-blurred windows are not in scope by default; give them an `alpha`
override to opt in. The root surface and up to three subsurfaces are sampled
(coverage is their maximum alpha), which covers GTK4 graphics offload and video
players. If no surface texture is usable in a frame (no texture yet, YUV
content, rotated buffer, opaque pixel format) the window falls back to the
geometric path. Each switch between the alpha and geometric path is logged
once per window at info level, prefixed `[alpha-diag]`, in the KWin journal.

### Full-blur alpha
Window alpha at which blur coverage reaches 100% (default 0.5). Lower it if a
very translucent window loses blur; raise it to harden edges.

# Force blur

### Classes of windows to force blur:

Window classes that should be force blurred (or excluded if `Blur all except matching` is enabled).
One window class per line.

> [!NOTE]
> Better Blur DX respects blur regions set by windows themselves (e.g. Plasma surfaces).
> Force blur settings are ignored in these cases.

### Blur window decorations as well

Whether to blur window decorations, including borders.
Enable this if your window decoration doesn't support blur.

This option will override the blur region specified by the decoration.

### Blur menus / Blur docks

Allows menus / docks to be blurred.
By default these are filtered from the list of blurred windows.

> [!NOTE]
> This only *allows* these to be blurred.
> You still need to add them to the force blurred window classes or enable `Blur all except matching`.

# D-Bus

The effect exposes `org.kde.KWin.Effects.BetterBlurDX` at
`/org/kde/KWin/Effects/BetterBlurDX` on the session bus (service
`org.kde.KWin`).

### Per-surface shapes

A client can publish the blur shape for all windows of one `app_id`
(resource class) at runtime, on top of the configured window overrides.
Values live in memory only; after an effect reload the
`surfaceShapesReset` signal fires and clients must publish again.

```
setSurfaceShape(appId, kind, radius, exponent, arrowCenterX, inset) -> bool
clearSurfaceShape(appId) -> bool
surfaceShapes() -> [string]
```

- `kind`: `inherit` (or empty), `rounded`, `squircle`, `squircle-flat`,
  `pointed`, `alpha`, `region`. Same meaning as the override table's shape
  field; `rounded` forces a plain rounded rectangle.
- `radius`: corner radius in logical px. `< 0` inherits.
- `exponent`: superellipse exponent (2–8) for squircle bodies and pointed
  bodies. `<= 0` inherits.
- `arrowCenterX`: pointed mask arrow tip, frame-local logical x. `< 0`
  inherits, which falls back to the position mined from the client's blur
  region.
- `inset`: mask inset in logical px (squircle plate, pointed body). `< 0`
  inherits.

Anything not published keeps coming from the config override that matches
the class (exact, regex or `$blank`) or from the globals.

```sh
# squircle plate, radius 32, exponent 3.4, default inset
qdbus6 org.kde.KWin /org/kde/KWin/Effects/BetterBlurDX setSurfaceShape surfboard-plate squircle 32 3.4 -1 -1
# move the tooltip arrow to x=57 without touching anything else
qdbus6 org.kde.KWin /org/kde/KWin/Effects/BetterBlurDX setSurfaceShape surfboard-tooltip inherit -1 -1 57 -1
qdbus6 org.kde.KWin /org/kde/KWin/Effects/BetterBlurDX surfaceShapes
qdbus6 org.kde.KWin /org/kde/KWin/Effects/BetterBlurDX clearSurfaceShape surfboard-plate
```

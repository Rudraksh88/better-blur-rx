# Better Blur RX

Better Blur RX is a fork of [Better Blur DX](https://github.com/xarblu/kwin-effects-better-blur-dx)
for KDE Plasma 6 on Wayland. On top of everything DX does, the blur behind a
window can follow the window's own alpha channel, and clients can publish the
exact shape of their blur over D-Bus: squircles, pointed tooltips, menus, docks.

It was built as the compositor side of [Surfboard](https://github.com/Rudraksh88/surfboard),
a dock for KDE Plasma, but nothing in it is specific to Surfboard: any window can use it.

The line it descends from: KWin's Blur effect, then [Better Blur](https://github.com/taj-ny/kwin-effects-forceblur)
by taj-ny, then Better Blur DX by Xarblu, then this fork. RX branched from DX's
`main` of 2026-07-11 and has diverged a long way since.

## What RX adds

Everything Better Blur DX has (force blur; blur brightness, contrast and
saturation; corner radius; refraction; DX's fixes for Plasma blur bugs), plus:

- **Blur shaped by window alpha.** Undecorated windows that ask for blur
  themselves (CSD apps, layer-shell panels and docks) get blur cut to their
  own antialiased silhouette, instead of the integer rectangles the Wayland
  blur protocol carries. Rounded and irregular corners need no per-app
  settings. Settings → General → Corners → "Shape blur by window alpha".
- **Shapes published at run time over D-Bus.** A client sets its blur shape
  per app ID (`setSurfaceShape`): rounded, squircle, flat-bottomed squircle,
  pointed with an arrow it can move (`setSurfaceArrow`), its own alpha, or an
  alpha threshold, each with an optional inset.
  `capabilities()` tells a client what this build supports.
- **Antialiased masks** for squircle windows, pointed tooltips, and menus and
  combo-box popups.
- **Backdrop probes.** A client can ask for the brightness of the blurred
  backdrop behind a strip or a set of points of its surface
  (`setSurfaceProbe`, `setSurfaceProbePoints`), to switch its text or
  indicators between light and dark.
- **Fades follow the window.** Blur opacity follows `wp_alpha_modifier`, so
  windows that fade with it fade their blur too.
- **Per-window overrides** with a rule editor and a window picker, on a
  redesigned settings page.
- **Fixes:** blur stays during window transforms and while a window closes,
  is never drawn for a closed window's remnant, and refreshes its geometry
  deterministically.

[CHANGELOG.md](CHANGELOG.md) has the details, [docs/surfboard-integration.md](docs/surfboard-integration.md)
the D-Bus contract, and [docs/configuration.md](docs/configuration.md) the settings.

## Compatibility

- **KDE Plasma 6 on Wayland.** Tested on KWin 6.8.2. The build refuses KWin
  older than 6.4; versions before 6.8 are untested.
- **X11** may build with `-DBBDX_X11=ON` but is untested, and the
  alpha-shaped and D-Bus features are Wayland-only.
- **Rebuild after every KWin update.** KWin effects only work with the exact
  KWin version they were built for. KWin also needs a restart (log out and
  back in) to load an updated effect; a first install doesn't.

### It replaces Better Blur DX

RX keeps DX's plugin ID (`better_blur_dx`), settings and D-Bus interface
(`org.kde.KWin.Effects.BetterBlurDX` on `/org/kde/KWin/Effects/BetterBlurDX`).
It installs over DX, keeps your DX settings, and clients written for DX keep
working. The two can't be installed side by side: remove a DX package first,
such as `kwin-effects-better-blur-dx` from the AUR.

## Installation

There are no packages yet; build it from source.

### Dependencies

CMake, Extra CMake Modules, Qt 6, KDE Frameworks 6 and KWin's development
files. On Arch Linux:

```sh
sudo pacman -S --needed base-devel git extra-cmake-modules qt6-tools kwin
```

The [Better Blur DX README](https://github.com/xarblu/kwin-effects-better-blur-dx#dependencies)
lists the packages for Debian, Ubuntu, Fedora and openSUSE; RX needs the same.

### Building

```sh
git clone https://github.com/Rudraksh88/better-blur-rx
cd better-blur-rx
cmake -S . -B build -DCMAKE_INSTALL_PREFIX=/usr
cmake --build build --parallel "$(nproc)"
sudo cmake --install build
```

After a KWin update, delete `build` and run these again, then log out and back in.

## Usage

This effect conflicts with KWin's own Blur effect and other effects that replace it.

1. Open *System Settings → Desktop Effects*.
2. Turn off any other blur effect.
3. Turn on *Better Blur RX*.

### Window transparency

Blur only shows behind translucent windows. Make a window translucent with:

- a transparent theme, if the program supports one;
- a transparent color scheme, such as [Alpha](https://store.kde.org/p/1972214);
- a window rule that lowers the window's opacity.

### Window classes

Force blur and overrides match windows by class. To find a window's class:

- Run `qdbus org.kde.KWin /KWin org.kde.KWin.queryWindowInfo` and click the
  window; use *resourceClass* or *resourceName*.
- Or right-click its title bar, choose *More Actions → Configure Special
  Application Settings*, and read *Window class (application)*. For a class
  with a space, such as *Navigator firefox*, use either word.

## Known issues

- **Other blur effects:** don't run KWin's Blur next to this effect. Some
  windows get blurred twice and look wrong.
- **Wobbly Windows:** the blur covers the window's rectangle, so it either
  bleeds past a wobbling window or is skipped while the wobble lasts.
- **Cursor latency on Wayland:** with many windows open, the GPU load can
  delay or stutter the cursor. Setting `KWIN_DRM_NO_AMS=1` helps; if not, also
  try `KWIN_FORCE_SW_CURSOR=0` or `KWIN_FORCE_SW_CURSOR=1`. Intel GPUs use a
  software cursor by default because of
  [this bug](https://gitlab.freedesktop.org/drm/intel/-/issues/9571).

## Credits

- The KWin development team, for the Blur effect this is built on.
- [taj-ny/kwin-effects-forceblur](https://github.com/taj-ny/kwin-effects-forceblur): the original Better Blur.
- [xarblu/kwin-effects-better-blur-dx](https://github.com/xarblu/kwin-effects-better-blur-dx):
  Better Blur DX, which this forks, with contributions from DaddelZeit and iGerman00.
- [a-parhom/LightlyShaders](https://github.com/a-parhom/LightlyShaders): the CMakeLists.txt files.

## License

GPL-3.0 ([LICENSE](LICENSE)). Sources taken from KWin carry their own
`GPL-2.0-or-later` headers.

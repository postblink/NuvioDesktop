# Changelog

This changelog covers the **unofficial Linux fork** only. It tracks
[NuvioMedia/NuvioDesktop](https://github.com/NuvioMedia/NuvioDesktop) and documents
the Linux-specific work layered on top.

## [0.1.8-alpha] — Linux

### Upstream sync
- Merged `upstream/Dev` (22 commits). Adopted upstream desktop fixes: player **arrow-key
  handling**, **episode stream rows**, and **addon URL encoding**; the detail/meta redesign
  (wide cast & production layouts, `DesktopDetailHero`, reworked home hero); and stability
  fixes — the **watch-progress concurrency crash**, multiple iOS crashes, plugin-runtime
  dispatcher isolation, and deleted-remote-account handling. The Linux Compose player path is
  untouched by the merge (it lives in Linux-only files).

### Fixed
- **Desktop build:** upstream's watch-progress concurrency fix uses `kotlinx.atomicfu.locks`,
  which wasn't on the desktop compile classpath (upstream builds the mobile/web targets, so it
  never surfaced there). Added the `kotlinx-atomicfu` dependency (`0.27.0`, aligned with
  coroutines `1.10.2`) so `commonMain` compiles for the desktop target.

## [Unreleased] — Linux port

### Upstream sync
- Merged `upstream/Dev` (now on par, **0 behind**; tracks 0.1.6-alpha). Upstream's desktop
  fullscreen controls live in the WebView overlay (Windows/macOS), so the Linux Compose
  fullscreen pill + keyboard handling are retained, not duplicated. Adopted upstream features:
  NVIDIA RTX VSR (native path), `PlayerResizeMode.Stretch`, volume get/set, cursor-activity
  callback, and the desktop subtitle font range (6..40). The Linux JNI bridge was extended to
  match upstream's new surface: `create()` accepts (and ignores) the VSR flag, `setVolume`/
  `volume` are implemented against mpv's `volume` property, and `Stretch` maps to
  `keepaspect=no`.

### Player architecture (final)
- Linux in-app playback renders **mpv frames in software into a Compose `Canvas`**
  (`NativePlayerBridge.renderFrame` + mpv's SW render API, `LinuxComposePlayer.kt`), with the
  app's shared Compose `PlayerControlsShell` drawn over them — the same model the Android
  target uses. This replaces the Windows/macOS native-window + native-overlay pattern, which
  is fundamentally incompatible with X11 (no compositing of overlay windows over the embedded
  video). The earlier wid-embedding + WebKitGTK / GtkGLArea / floating-window overlay attempts
  (documented below) each hit X11/GLX/occlusion walls and were abandoned.

### Added
- Native Linux desktop video playback: an embedded **libmpv** player bridge
  (`composeApp/src/desktopMain/native/linux/player_bridge.cpp`) implementing the full
  `NativePlayerBridge` JNI contract — transport, state queries, and subtitle delay/style.
- `LinuxAwtViewResolver`: resolves the host AWT `Canvas` X11 window id (XID) for mpv `wid`
  embedding.
- Gradle `buildLinuxPlayerBridge` task (g++ + `pkg-config mpv`, `$ORIGIN` rpath), with
  `desktopJar` bundling and run/package task wiring.
- `--add-opens=java.desktop/sun.awt.X11=ALL-UNNAMED` for the X11 peer reflection.
- **AppImage packaging**: `packageLinuxAppImage` Gradle task wraps the jpackage app image
  (`createDistributable`) into a portable AppImage via `scripts/build-appimage.sh`
  (downloads/caches `appimagetool`, builds the AppDir + `.desktop` + `AppRun`). libmpv is
  resolved from the host at runtime, not bundled — the README documents the dependency.
- Audio/subtitle **track pickers** and subtitle **styling** wired into the Compose Linux path
  (`LinuxComposePlayer.kt`): `getAudioTracks`/`getSubtitleTracks` decode the bridge's track
  JSON, `selectAudioTrack`/`selectSubtitleTrack` resolve the picker index to an mpv track id,
  and `applySubtitleStyle` maps `SubtitleStyleState` (color, opacity, font size, position) to
  mpv subtitle properties.

### Fixed
- Linux fullscreen exit was unreliable (Esc/F/pill sometimes failed; Esc often backed out of
  the player instead). Three stacked causes: the player tracked its own `isFullscreen` flag
  that F11 (the global key dispatcher) never updated; `WindowPlacement.Fullscreen` is written
  back to Maximized/Floating by the WM after the transition, so state checks read false; and
  the transition recreates the AWT window peer, dropping keyboard focus so F/Esc/Space stopped
  reaching the player. Now `DesktopAppFullscreenController` owns the Linux fullscreen state,
  every input (F, Esc, pill, F11) resolves against one source of truth via a new
  `PlayerEngineController.isHostFullscreen()`, and window focus is re-asserted after each
  toggle. (macOS/Windows keep their native placement path.)
- Linux was routed to the "in-app playback not available" stub instead of the native player.
- Player never attached: heavyweight AWT `Canvas` in a Compose `SwingPanel` doesn't reliably
  receive `paint()` on X11 — first-paint notification now also fires from resize/show events.
- libmpv aborted the process under a non-C numeric locale (`setlocale(LC_NUMERIC, "C")`).
- Malformed `loadfile` (options in the integer index slot); `pause`/`start` now set as options.
- mpv opened a separate window on Wayland (ignored the X11-only `wid`); forced an X11 context.
- Black video / audio-only: EGL couldn't make its GL context current on the foreign AWT
  window — switched the GPU context to **GLX**.
- Black artifacting from AWT repainting over the mpv surface (Linux-gated `ignoreRepaint`).
- Subtitle Style panel showed no selected-swatch highlight once Text Opacity was changed:
  `Color` equality is alpha-sensitive, so a stored color with custom alpha never matched the
  opaque swatches. Selection now compares hue only, and the selected swatch is enlarged with a
  thicker accent ring so the active color is unmistakable (shared UI — benefits all platforms).

### Known gaps / Linux caveats
- Software rendering is **CPU-composited** (hwdec still decodes in hardware) — fine for 1080p,
  heavier for 4K than the GPU path on Windows/macOS.
- The AppImage requires **system libmpv** (`libmpv.so.2`) on the target machine; it is not
  bundled. A fully self-contained AppImage (bundled libmpv + codecs) is a possible follow-up.

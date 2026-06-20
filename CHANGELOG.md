# Changelog

This changelog covers the **unofficial Linux fork** only. It tracks
[NuvioMedia/NuvioDesktop](https://github.com/NuvioMedia/NuvioDesktop) and documents
the Linux-specific work layered on top.

## [Unreleased] — Linux port

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
- Audio/subtitle **track pickers** and subtitle **styling** wired into the Compose Linux path
  (`LinuxComposePlayer.kt`): `getAudioTracks`/`getSubtitleTracks` decode the bridge's track
  JSON, `selectAudioTrack`/`selectSubtitleTrack` resolve the picker index to an mpv track id,
  and `applySubtitleStyle` maps `SubtitleStyleState` (color, opacity, font size, position) to
  mpv subtitle properties.

### Fixed
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
- **AppImage** packaging pending; desktop fullscreen/chrome niceties unverified on Linux.
- Dead code from the abandoned overlay approaches remains in the bridge (cleanup pending).

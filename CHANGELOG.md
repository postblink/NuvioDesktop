# Changelog

This changelog covers the **unofficial Linux fork** only. It tracks
[NuvioMedia/NuvioDesktop](https://github.com/NuvioMedia/NuvioDesktop) and documents
the Linux-specific work layered on top.

## [Unreleased] — Linux port

### Added
- Native Linux desktop video playback: an embedded **libmpv** player bridge
  (`composeApp/src/desktopMain/native/linux/player_bridge.cpp`) implementing the full
  `NativePlayerBridge` JNI contract — transport, state queries, and subtitle delay/style.
- `LinuxAwtViewResolver`: resolves the host AWT `Canvas` X11 window id (XID) for mpv `wid`
  embedding.
- Gradle `buildLinuxPlayerBridge` task (g++ + `pkg-config mpv`, `$ORIGIN` rpath), with
  `desktopJar` bundling and run/package task wiring.
- `--add-opens=java.desktop/sun.awt.X11=ALL-UNNAMED` for the X11 peer reflection.

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

### Known gaps (later phases)
- In-player controls overlay (WebKitGTK), audio/subtitle track pickers, window chrome /
  fullscreen X11 hints, and AppImage packaging are not yet implemented.

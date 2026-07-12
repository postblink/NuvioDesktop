# Changelog

This changelog covers the **unofficial Linux fork** only. It tracks
[NuvioMedia/NuvioDesktop](https://github.com/NuvioMedia/NuvioDesktop) and documents
the Linux-specific work layered on top.

Each **Upstream sync** entry states the upstream ref being tracked as of that merge —
either their latest version tag, or (when `Dev` has moved past their last tag) that tag
plus the commit count/SHA on top, e.g. `0.1.11-alpha + 25 commits (6ff150fd)`.

## [0.1.20-alpha] — Linux

### Upstream sync
- Merged `upstream/Dev` (48 commits) through official **`0.1.12-alpha` (`64dcc8be`)**.
  Notable shared updates include the Navigation 3 migration, foreground and watch-progress
  sync fixes, profile-safe library state, debrid-first stream resolution, configurable
  poster/card depth and zoom actions, the content-warnings preference, and Dutch, Romanian,
  and Vietnamese translations. Desktop also adopts upstream's removal of the temporary
  playback proxy; macOS receives its new bundled dynamic player runtime.
- Preserved the Linux Compose/libmpv player and AppImage packaging, the dedicated QuickJS
  dispatcher that prevents plugin fan-out deadlocks, search-request deduplication, and the
  fork-specific Trakt and Premiumize OAuth configuration.

## [0.1.19-alpha] — Linux

### Fixed
- **Premiumize sign-in now works in this fork's builds.** Premiumize uses an OAuth
  device-code flow gated on `PREMIUMIZE_CLIENT_ID`, which was empty in our builds — so
  **Settings → Debrid → Premiumize → Connect** failed with a "missing configuration" error.
  As with Trakt, the official builds bake in NuvioMedia's private Premiumize client, which
  isn't published, so this fork registers its **own** Premiumize OAuth client (device-code
  flow, `client_id` only — no secret) **until an official Linux desktop build ships**, at
  which point it'll defer to the official integration. See the README **Premiumize** section.

## [0.1.18-alpha] — Linux

### Added
- **Trakt sign-in and syncing now work in this fork's builds**, via a **separate,
  fork-specific Trakt OAuth application** — not the official Nuvio one. The official
  desktop builds bake in NuvioMedia's private Trakt credentials, which aren't published,
  so we don't have access to the official OAuth implementation as it stands. **Until an
  official Linux desktop build ships**, this fork registers its own Trakt app so scrobbling
  and watched/collection sync still function; when an official Linux build launches, this
  fork will defer to the official integration. Desktop uses Trakt's device-code flow
  (**Settings → Trakt → Connect** → `trakt.tv/activate` + a short code), identical across
  Windows/macOS/Linux. The fork app requests **scrobble** permission only (no checkin).
  See the README **Trakt** section for details.

## [0.1.17-alpha] — Linux

### Fixed
- **Search list no longer crashes when two catalogs resolve to the same
  `(addon, type, catalogId)`.** Duplicate tuples produced duplicate section keys
  (`<id>:search:<type>:<catalogId>:<query>`); as results streamed in, the duplicate
  LazyColumn/subcompose keys crashed the search list during Compose's lookahead pass
  (surfaced as "layout state is not idle before measure starts", masking the real
  "Key … was already used"). `buildSearchRequests` now drops duplicates at the source —
  same tuple is the same fetch. (Ported from the HTPC fork; a generic `commonMain` bug
  that also affects upstream.)

## [0.1.16-alpha] — Linux

### Upstream sync
- Merged `upstream/Dev` (80 commits) — synced through **`0.1.11-alpha` + 25 untagged
  commits (`6ff150fd`)**, upstream's latest tag lags their `Dev` branch by that much.
  Notable: **cmp-rewrite performance pass** merged into desktopweb (reduced Compose
  recomposition hotspots, home tab stays mounted), **P2P streaming for desktop**,
  **Sentry diagnostics**, **Trakt credential sync across clients**, macOS Now Playing
  support, deeplinks for addons/detail screens, desktop windowed-geometry-during-fullscreen
  fix, Windows display-sleep inhibit during playback, macOS TorrServer resource bundling,
  and continue-watching/home-catalog sync-key fixes. Croatian, Greek, Hungarian, and
  additional Italian translation passes.

### Changed
- **Desktop addon HTTP bridge rewritten onto OkHttp** (upstream). This supersedes the
  0.1.15 JDK-restricted-headers fix — OkHttp doesn't reject hop-by-hop headers the way
  `java.net.http` did, so the workaround is gone rather than reapplied; upstream's version
  also adds response-body truncation (1 MiB cap) and a custom IPv4-first DNS resolver.

## [0.1.15-alpha] — Linux

### Upstream sync
- Merged `upstream/Dev` (20 commits). Notable: **realtime sync invalidation** (with
  self-originated event filtering), **Compose Multiplatform beta bump** (1.12.0-beta01),
  runtime configuration refactor, horizontal scrolling in the profile switcher tab,
  Android dominant-colour extraction fix, and French/Spanish translation passes. The
  plugin runtime keeps the dedicated dispatcher (verified post-merge); Linux player
  bridge untouched.

### Fixed
- **Desktop addon HTTP bridge no longer aborts requests carrying hop-by-hop headers.**
  Plugins routinely send `Connection: keep-alive`; `java.net.http` rejects restricted
  headers with `IllegalArgumentException`, killing the whole fetch. Restricted headers
  are now dropped before the request is built. (Pre-existing bug, surfaced during the
  0.1.15 smoke test — 39 occurrences in one session.)

## [0.1.14-alpha] — Linux

### Upstream sync
- Merged `upstream/Dev` (7 commits). Watch-state improvements: **home startup stabilised
  and watched badges refresh correctly**, **series watched-state reconciliation outside the
  details screen**, **snapshot sync on mobile startup**, and poster-badge refresh for series.
  Desktop: **hero section logo hover surface removed**. Dominant-colour background stability
  fix (follow-up to 0.1.13).

## [0.1.13-alpha] — Linux

### Upstream sync
- Merged `upstream/Dev` (41 commits). Notable: **support for up to 6 user profiles**
  (sidebar overlap fix included), **dominant-colour dynamic background** on meta detail
  screens, Bulgarian and Slovak localisations, external-player skip-segment support,
  stream-loading preservation during external-player prep, source-change playback
  recreation fix, and assorted mobile/player refinements. No changes to the plugin runtime
  or Linux player bridge.

## [0.1.12-alpha] — Linux

### Upstream sync
- Merged `upstream/Dev` (8 commits across two syncs since 0.1.11). Desktop player &
  UI fixes: **player controls now re-attach after advancing to the next episode**,
  **playback volume persists across sessions**, **maximised-window contents no longer
  buried under the taskbar**, Windows borderless-fullscreen focus fix, macOS spatial
  audio, and a stream-chip drag-scroll fix. No changes to the plugin runtime — the
  desktop freeze fix (`JsRuntime` dedicated dispatcher) and the Linux player bridge
  are untouched.

## [0.1.11-alpha] — Linux

### Upstream sync
- Merged `upstream/Dev` (~89 commits). Notably a **modular plugin-runtime rewrite**
  (`PluginRuntime.kt` → `plugins/runtime/` with `js/JsRuntime.kt`, `network/FetchBridge.kt`,
  crypto/dom/wasm bridges, URL bridge, enhanced polyfills), desktop fullscreen HUD control
  fixes, home-collections refresh fix, and assorted mobile/iOS fixes. Bumped `kotlinx-atomicfu`
  to upstream's `0.30.0`.

### Fixed
- **Re-applied the plugin-runtime freeze fix to the new modular runtime.** Upstream's rewrite
  moved QuickJS into `js/JsRuntime.kt`, still defaulting to `Dispatchers.Default` — which
  reintroduces the desktop UI freeze (see 0.1.10) at high plugin fan-out. `JsRuntime` now
  defaults to a dedicated isolated `newFixedThreadPoolContext` pool, so plugin blocking can't
  starve the shared dispatchers. (Reported upstream separately; their inline-`runBlocking`
  change is only a partial mitigation.)

## [0.1.10-alpha] — Linux

### Fixed
- **Desktop UI hard-freeze + "no streams found"** when opening a title with multiple
  stream plugins. The plugin runtime ran QuickJS and its *synchronous* native fetch on
  `Dispatchers.Default`; a fan-out of scrapers (16 in the reported case) parked every
  scheduler thread in `runBlocking`, starving `Dispatchers.Default`. On desktop that
  deadlocks the UI thread, because Compose's `stringResource()` does a blocking resource
  load on the EDT — so the whole app froze (no back, no window close) and no streams
  resolved. The QuickJS runtime now runs on a dedicated, isolated thread pool
  (`newFixedThreadPoolContext`), with the HTTP request itself on `Dispatchers.IO`, so
  plugin blocking can never starve the shared dispatchers. Regression from the upstream
  plugin-dispatcher changes (reported upstream). Diagnosed from a JVM thread dump.

## [0.1.9-alpha] — Linux

### Fixed
- **Account login (hotfix).** Releases 0.1.6–0.1.8 shipped with an empty backend
  config, so sign-in resolved to `https://localhost/auth/v1/token` and failed with
  "Connection refused." The build now supplies Nuvio's hosted backend URL and the
  **publishable** client key — values Nuvio publishes for third-party clients at
  <https://nuvio.tv/docs> — via the gitignored `local.properties`. Verified against the
  live backend (`health-check` OK; auth endpoint reachable; session loads from storage).

### Build
- **Packaging guardrail.** `createDistributable` / `createReleaseDistributable` now fail
  fast when `SUPABASE_URL` is blank, so an unconfigured (localhost-login) artifact can no
  longer be packaged or shipped. Dev `run` is unaffected.

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

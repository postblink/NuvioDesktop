<div align="center">

  <img src="composeApp/src/commonMain/composeResources/drawable/app_logo_wordmark.png" alt="Nuvio" width="300" />
  <br />
  <br />

  [![Contributors][contributors-shield]][contributors-url]
  [![Forks][forks-shield]][forks-url]
  [![Stargazers][stars-shield]][stars-url]
  [![Issues][issues-shield]][issues-url]
  [![License][license-shield]][license-url]

  <p>
    A desktop media app for Windows, macOS, and Linux.
    <br />
    Browse, organize, and play media from sources you add.
  </p>

</div>

> [!NOTE]
> **Unofficial Linux port.** This fork adds a native Linux build of Nuvio Desktop while
> aiming to stay as faithful as possible to the official upstream branch. It tracks
> [NuvioMedia/NuvioDesktop](https://github.com/NuvioMedia/NuvioDesktop) and is intended as a
> stopgap until an official Linux release lands, at which point this port defers to it.
>
> 📥 **Download the latest Linux AppImage:** [postblink/NuvioDesktop releases](https://github.com/postblink/NuvioDesktop/releases) — newest build is at the top.

## ⚠️ Alpha Software — Testers Only

Nuvio Desktop is currently in alpha and is intended only for testers. It is under active development and is not suitable for daily use.

Expect breaking changes with every update. Features, settings, stored data, and compatibility may change or stop working without notice. Do not rely on this build as your primary media app, and report any issues you encounter during testing.

## About

Nuvio Desktop is a media client for browsing metadata, managing collections and watch progress, downloading media, and playing streams from user-installed extensions or user-provided sources.

## Installation

Download the latest desktop build from [GitHub Releases](https://github.com/NuvioMedia/NuvioDesktop/releases/latest).

Release packages are provided for supported desktop platforms:

- Windows: MSI installer
- macOS: DMG installer
- Linux: DEB package or AppImage, when available

> [!IMPORTANT]
> **Linux runtime dependency:** in-app playback uses **libmpv**, which is not bundled.
> Install your distro's mpv/libmpv package before running (e.g. `mpv` / `libmpv2` /
> `libmpv-dev`). The app resolves `libmpv.so.2` from the system at runtime.

## Trakt

> [!NOTE]
> **This unofficial Linux fork uses its own, fork-specific Trakt OAuth application** —
> not the official Nuvio one. The official desktop builds bake in NuvioMedia's private
> Trakt OAuth credentials, which aren't published, so we don't have access to the official
> OAuth implementation as it stands. **Until an official Linux desktop app launches**, this
> fork registers a separate Trakt app so Trakt syncing (scrobbling, watched/collection
> sync) still works. Functionality is identical — only the registered OAuth application
> differs — and when an official Linux build ships, this fork will defer to the official
> Trakt integration.

Sign-in uses Trakt's device-code flow: **Settings → Trakt → Connect** opens
`trakt.tv/activate` in your browser and shows a short code to enter. No callback/redirect
is involved, so it works the same on Windows, macOS, and Linux.

## Premiumize

> [!NOTE]
> Same situation as Trakt: **this fork uses its own Premiumize OAuth client**, not the
> official Nuvio one, because NuvioMedia's Premiumize client isn't published. It's a
> **temporary measure until an official Linux desktop app launches**, after which the fork
> will defer to the official integration. Premiumize sign-in uses a device-code flow
> (**Settings → Debrid → Premiumize → Connect** → `premiumize.me/device` + a short code);
> the fork client uses `client_id` only (no secret).

## Development

```bash
git clone https://github.com/NuvioMedia/NuvioDesktop.git
cd NuvioDesktop
```

Run from source:

```bash
./gradlew :composeApp:run
```

### Build configuration (optional secrets)

Optional API credentials are read from a **gitignored** `local.properties` at the repo
root by `GenerateRuntimeConfigsTask`; every key defaults to empty, so an empty file still
builds and launches the UI. To enable Trakt in your own build, register a Trakt app at
<https://trakt.tv/oauth/applications> and set:

```properties
TRAKT_CLIENT_ID=<your client id>
TRAKT_CLIENT_SECRET=<your client secret>
```

For Premiumize, register a client at <https://www.premiumize.me/registerclient> and set
`PREMIUMIZE_CLIENT_ID` (device-code flow — `client_id` only, no secret):

```properties
PREMIUMIZE_CLIENT_ID=<your client id>
```

The Trakt redirect URI is unused by the desktop device-code flow. Because credentials baked
into a distributed binary are only semi-secret, **use your own Trakt/Premiumize apps — never
reuse another project's credentials.**

On Windows PowerShell:

```powershell
.\gradlew.bat :composeApp:run
```

Build a release package for the current host:

```bash
./gradlew :composeApp:packageReleaseDistributionForCurrentOS
```

Platform-specific packaging:

```bash
# Windows
./gradlew :composeApp:packageReleaseMsi --rerun-tasks

# macOS
./scripts/build-macos-release-dmgs.sh --package-only

# Linux (DEB)
./gradlew :composeApp:packageReleaseDeb

# Linux (AppImage) — wraps createDistributable; downloads appimagetool on first run.
# Requires system libmpv at runtime (not bundled). Output:
#   composeApp/build/compose/binaries/main/appimage/Nuvio-<version>-linux-<arch>.AppImage
./gradlew :composeApp:packageLinuxAppImage
```

## Project Structure

- `composeApp/` contains the app code.
- `composeApp/src/commonMain/` contains shared UI, features, repositories, and platform-agnostic logic.
- `composeApp/src/desktopMain/` contains desktop-specific integrations.
- `composeApp/Configuration/DesktopVersion.properties` contains the desktop release version and build code.

## Versioning

Desktop versions are set in `composeApp/Configuration/DesktopVersion.properties`.

```properties
VERSION_NAME=0.1.1-alpha
VERSION_CODE=1
```

Use the version helper when changing desktop release versions:

```bash
./scripts/set-version.sh --desktop 0.1.2-alpha --desktop-code 2
./scripts/set-version.sh --show
```

## Legal & DMCA

Nuvio functions solely as a client-side interface for browsing metadata and playing media provided by user-installed extensions and/or user-provided sources. It is intended for content the user owns or is otherwise authorized to access.

Nuvio is not affiliated with any third-party extensions, catalogs, sources, or content providers. It does not host, store, or distribute any media content.

For comprehensive legal information, including our full disclaimer, third-party extension policy, and DMCA/Copyright information, please visit our [Legal & Disclaimer Page](https://nuvioapp.space/legal).

## Built With

- Kotlin Multiplatform
- Compose Multiplatform
- Kotlin
- Compose Desktop packaging
- Native desktop player integrations

## Star History

<a href="https://www.star-history.com/#NuvioMedia/NuvioDesktop&type=date&legend=top-left">
 <picture>
   <source media="(prefers-color-scheme: dark)" srcset="https://api.star-history.com/svg?repos=NuvioMedia/NuvioDesktop&type=date&theme=dark&legend=top-left" />
   <source media="(prefers-color-scheme: light)" srcset="https://api.star-history.com/svg?repos=NuvioMedia/NuvioDesktop&type=date&legend=top-left" />
   <img alt="Star History Chart" src="https://api.star-history.com/svg?repos=NuvioMedia/NuvioDesktop&type=date&legend=top-left" />
 </picture>
</a>

<!-- MARKDOWN LINKS & IMAGES -->
[contributors-shield]: https://img.shields.io/github/contributors/NuvioMedia/NuvioDesktop.svg?style=for-the-badge
[contributors-url]: https://github.com/NuvioMedia/NuvioDesktop/graphs/contributors
[forks-shield]: https://img.shields.io/github/forks/NuvioMedia/NuvioDesktop.svg?style=for-the-badge
[forks-url]: https://github.com/NuvioMedia/NuvioDesktop/network/members
[stars-shield]: https://img.shields.io/github/stars/NuvioMedia/NuvioDesktop.svg?style=for-the-badge
[stars-url]: https://github.com/NuvioMedia/NuvioDesktop/stargazers
[issues-shield]: https://img.shields.io/github/issues/NuvioMedia/NuvioDesktop.svg?style=for-the-badge
[issues-url]: https://github.com/NuvioMedia/NuvioDesktop/issues
[license-shield]: https://img.shields.io/github/license/NuvioMedia/NuvioDesktop.svg?style=for-the-badge
[license-url]: https://github.com/NuvioMedia/NuvioDesktop/blob/main/LICENSE

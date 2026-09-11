# Nuvio Desktop Linux — Archived

> **This repository is retired and read-only.**
>
> Download the maintained Nuvio Desktop release from
> **[NuvioMedia/NuvioDesktop Releases](https://github.com/NuvioMedia/NuvioDesktop/releases/latest)**.

This was an unofficial Linux port of Nuvio Desktop, created before upstream provided Linux builds.
Upstream now publishes official Linux packages, so this fork is no longer maintained.

The official release provides AppImage packages with zsync delta updates, DEB, RPM, and Flatpak
packages, plus published checksums. This fork only produced a plain AppImage.

## Migration

If you still use this fork's AppImage:

1. Install the official Nuvio Desktop release.
2. Remove the old AppImage when you have confirmed the official build works.
3. Sign in to restore your library, watch progress, and settings from your Nuvio account and
   profile data.
4. Reconnect Trakt if necessary. This fork used its own Trakt OAuth application.
5. Reconnect Premiumize if you used it. This fork used its own temporary client there as well.

You can revoke this fork's old Trakt authorization from
[Trakt application settings](https://trakt.tv/settings/applications).

## Historical scope

This fork reached `0.1.24-alpha` using its own version numbering. Its version numbers are
unrelated to upstream's and must not be compared with official releases.

The fork originally supplied a Linux AppImage and relied on the system `libmpv.so.2` runtime. Its
Linux player bridge was removed in `0.1.24-alpha` after upstream's own Linux player bridge
superseded it.

The repository remains available to preserve existing source and download links.

## Upstream and license

This project was derived from [NuvioMedia/NuvioDesktop](https://github.com/NuvioMedia/NuvioDesktop).

This repository is licensed under the [GNU General Public License v3.0](LICENSE). See the upstream
repository for maintained source code, releases, support, and current documentation.

# PosseTTD changelog

Changes relative to upstream OpenTTD 15.3. Newest first.

## Unreleased

### Phase 1, step 1: territory overlay in the main view (client-side, display only)
- New files in `src/possettd/` (`territory.cpp/.h`). Press **T** to toggle the overlay.
- Territory = station catchment areas weighted by the rating of cargo actually being served. Strongest company wins each tile; close runner-ups make the tile contested (striped). Borders drawn where territory changes.
- Recalculated every few game days on each client (never every frame); never saved, never networked, so it cannot desync.
- New client settings (Settings > Interface > General): overlay on/off, refresh interval, contested threshold.

### Phase 0: Setup and build pipeline
- Forked from OpenTTD 15.3.
- Rebranded: window title, main menu version label, "About" window, Windows exe properties, package name.
  Version string is now the git tag (e.g. `v0.1.0`), so it can never connect to official OpenTTD games.
- Executable renamed to `possettd.exe`.
- Settings and savegames now live in `Documents\PosseTTD` instead of `Documents\OpenTTD`.
- Added GitHub Actions workflow `possettd-release.yml`: pushing a tag `v*` builds a Windows x64 portable zip
  with OpenGFX, OpenSFX and OpenMSX bundled and attaches it to a GitHub Release. Upstream workflows moved to `.github/upstream-workflows/`
  (disabled) so they don't fail on this fork.

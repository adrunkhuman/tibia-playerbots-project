# Client Runtime

## Supported Bootstrap

The OTClient executable and Tibia DAT/SPR assets are intentionally excluded
from Git. Install and verify the pinned runtime after cloning:

```powershell
pwsh -File scripts/bootstrap-client.ps1
```

The script requires an authenticated GitHub CLI to download the private
executable release. It verifies SHA-256 for the executable, downloaded asset
archive, `Tibia.dat`, and `Tibia.spr`. Do not weaken or remove these checks.

Files are installed to the normal runtime locations:

- `client/otclient_gl_x64.exe`
- `client/data/things/860/Tibia.dat`
- `client/data/things/860/Tibia.spr`

Launch the isolated project profile with:

```powershell
client/launch-angelion-redemption.cmd
```

The profile name is `angelion-redemption`.

## Compatibility Contract

The project targets standard Tibia protocol 8.60. Preserve these verified
defaults unless intentionally changing gameplay behavior:

- Server endpoint `127.0.0.1:7171`, protocol `860`
- `force-new-walking-formula: true`
- `item-ticks-per-frame: 500`
- Right-click NPC talk enabled
- Isolated profile `angelion-redemption`

Do not enable Mateuzkl-specific or other custom packet extensions unless the
matching server implementation is added and both sides are tested together.
Preserve standard 8.60 feature boundaries in
`client/modules/game_features/features.lua`.

## Auto-Install Boundary

Client asset auto-installation is disabled for this project. The supported
runtime path is `scripts/bootstrap-client.ps1`.
`client/docs/client-assets-auto-install.md` documents an upstream capability,
not the enabled project bootstrap path.

If auto-installation is intentionally changed later, final paths must remain
OTClient-standard:

- `data/things/<version>/`
- `data/sounds/<version>/`
- Runtime extras in their expected locations

Do not introduce an alternate permanent runtime source of truth. Keep strict
manifest SHA-256 enabled, raw hash-mismatch fallback disabled, and preserve
desktop extraction and Android build compatibility.

## Repository Safety

Never commit `Tibia.dat`, `Tibia.spr`, OTClient executables, logs, screenshots,
minimap caches, or database volumes. Update the durable private release asset,
expected hashes, and documentation together when changing the executable or
Tibia assets.

The near-limit world map is a separate repository constraint:
`server/data/world/World.otbm` must move to Git LFS before it crosses GitHub's
100 MiB file limit.

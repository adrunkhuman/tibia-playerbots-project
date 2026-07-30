# Client runtime

## Bootstrap

The OTClient executable and Tibia DAT/SPR assets are excluded from Git. Install
and verify the pinned runtime with:

```powershell
pwsh -File scripts/bootstrap-client.ps1
```

GitHub CLI authentication is required only when the private executable must be
downloaded. The script verifies SHA-256 for the executable, asset archive,
`Tibia.dat`, and `Tibia.spr`; do not weaken these checks.

| File | Runtime path |
| ---- | ------------ |
| OTClient | `client/otclient_gl_x64.exe` |
| DAT | `client/data/things/860/Tibia.dat` |
| SPR | `client/data/things/860/Tibia.spr` |

Launch `client/launch-angelion-redemption.cmd`. The isolated profile is
`angelion-redemption`.

## Compatibility contract

The project targets standard Tibia protocol 8.60. Preserve these defaults unless
intentionally changing gameplay behavior:

| Setting | Value |
| ------- | ----- |
| Server | `127.0.0.1:7171` |
| Protocol | `860` |
| Walking | `force-new-walking-formula: true` |
| Item animation | `item-ticks-per-frame: 500` |
| NPC interaction | Right-click talk enabled |
| Profile | `angelion-redemption` |

Do not enable Mateuzkl-specific or other custom packet extensions without the
matching server implementation and cross-stack tests. Preserve standard 8.60
feature boundaries in `client/modules/game_features/features.lua`.

## Asset boundary

Client asset auto-installation is disabled. `scripts/bootstrap-client.ps1` is
the only supported runtime source; the client's auto-install documentation
describes an upstream capability, not project behavior.

Never commit `Tibia.dat`, `Tibia.spr`, OTClient executables, logs, screenshots,
or minimap caches. When changing the runtime, update the durable private release
asset, expected hashes, and this documentation together.

# Client runtime

## Bootstrap

The OTClient executable and Tibia DAT/SPR assets are excluded from Git. Install
and verify the runtime assets with PowerShell 7+ on Windows or Linux:

```powershell
pwsh -File scripts/bootstrap-client.ps1
```

On Windows, GitHub CLI authentication is required only when the pinned private
executable must be downloaded. On Linux, build `client/otclient` locally first;
bootstrap requires that file and leaves it untouched, including with `-Force`.
No Linux binary is published or downloaded, and the local build has no pinned
hash. The script preserves SHA-256 verification for the Windows executable,
asset archive, `Tibia.dat`, and `Tibia.spr`. `-Force` reinstalls assets on both
platforms and the executable on Windows only.

| File | Runtime path |
| ---- | ------------ |
| OTClient (Windows) | `client/otclient_gl_x64.exe` |
| OTClient (Linux local build) | `client/otclient` |
| DAT | `client/data/things/860/Tibia.dat` |
| SPR | `client/data/things/860/Tibia.spr` |

On Windows, launch `client/launch-angelion-redemption.cmd`. On Linux, run
`(cd client && ./otclient)` from a POSIX shell, or `Push-Location client;
./otclient; Pop-Location` in PowerShell. The isolated profile remains
`angelion-redemption`. Bootstrap does not validate native build dependencies,
executable permissions, display/input behavior, or in-game compatibility.

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
the supported asset installer (and Windows runtime source); the client's auto-install documentation
describes an upstream capability, not project behavior.

Never commit `Tibia.dat`, `Tibia.spr`, OTClient executables, logs, screenshots,
or minimap caches. When changing the pinned Windows runtime or assets, update
the durable private release asset, expected hashes, and this documentation together.

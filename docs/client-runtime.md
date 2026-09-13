# Client runtime

The repository does not track OTClient executables or Tibia DAT/SPR assets. Install the runtime from the repository root with PowerShell 7+:

```powershell
pwsh -File scripts/bootstrap-client.ps1
```

## Platform setup

| Platform | Before bootstrap | What bootstrap installs | Launch |
| --- | --- | --- | --- |
| Windows | Authenticate `gh` if the pinned private executable is absent | Verified `client/otclient_gl_x64.exe`, `Tibia.dat`, and `Tibia.spr` | `client/launch-angelion-redemption.cmd` |
| Linux | Build `client/otclient` locally | Verified `Tibia.dat` and `Tibia.spr`; the local executable is left untouched | `(cd client && ./otclient)` from a POSIX shell |

Use `-Force` to reinstall assets on either platform and the executable on Windows. The bootstrap script verifies the downloaded Windows executable, asset archive, and extracted assets with SHA-256. It does not validate Linux build dependencies, executable permissions, display/input behavior, or in-game compatibility.

Runtime assets are installed at `client/data/things/860/Tibia.dat` and `client/data/things/860/Tibia.spr`. Client auto-installation remains disabled; [`scripts/bootstrap-client.ps1`](../scripts/bootstrap-client.ps1) is the supported installer. Never commit client executables, DAT/SPR assets, logs, screenshots, or minimap caches.

## Protocol contract

The client targets standard Tibia protocol 8.60 at `127.0.0.1:7171` and uses the isolated profile `angelion-redemption`. Preserve the project's verified defaults unless intentionally changing gameplay: new walking formula enabled, item animation at 500 ticks per frame, and right-click NPC talk enabled.

Do not enable Mateuzkl-specific or other custom packet extensions without matching server support and cross-stack tests. Preserve the standard 8.60 feature boundaries in [`client/modules/game_features/features.lua`](../client/modules/game_features/features.lua).

A successful login alone does not establish compatibility. Protocol or gameplay-facing client changes require the manual checks in [Testing](testing.md).

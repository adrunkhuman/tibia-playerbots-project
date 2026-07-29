# Tibia Playerbots Project

Custom Tibia 8.60 development base combining:

- `server/`: Giorox Angelion real-map server on the TFS 1.5 downgrade.
- `client/`: OTClient Redemption configured for standard protocol 8.60.

The repositories are imported as squashed Git subtrees so server and client
changes can be committed atomically while retaining an upstream update path.

## Documentation

| Guide | Contents |
| ----- | -------- |
| [Playerbots](docs/playerbots.md) | Bot ownership, provisioning, behavior, navigation, looting, persistence, configuration, and telemetry |
| [Testing](docs/testing.md) | Server smoke checks, gameplay suites, regression modes, and client compatibility checks |
| [Client runtime](docs/client-runtime.md) | Pinned runtime bootstrap, protocol defaults, assets, profiles, and maintenance boundaries |

Inherited subtree documentation remains under `server/` and `client/`; the
guides above describe this integrated project's supported workflow.

## Prerequisites

- Docker Desktop with Docker Compose
- PowerShell 7+
- GitHub CLI authenticated with `gh auth login` for the private client runtime

## Quick Start

Install and verify the pinned client executable and Tibia assets:

```powershell
pwsh -File scripts/bootstrap-client.ps1
```

Start the disposable local server stack:

```powershell
docker compose -f server/compose.yaml up --build --detach
docker compose -f server/compose.yaml logs --follow server
```

Launch the client with `client/launch-angelion-redemption.cmd`. The server binds
ports `7171` and `7172` only to `127.0.0.1`.

## Local Accounts

| Account | Password | Character |
| ------- | -------- | --------- |
| `admin` | `admin` | `GOD Admin`, `Rook Tester` |
| `bot-one` | `bot-one` | `Bot One` (server-controlled) |

`Rook Tester` is a reproducible level-50 traversal character with sword and
shielding skills, backpack, plate armor, dark shield, fire sword, Boots of
Haste, rope, and shovel. The loadout is recreated with every fresh database.
`Bot One` starts at level 1 with a jacket, club, backpack, tools, 200 carried gp,
and 100 banked gp; the configured playerbot speed bonus remains enabled for
faster local observation.

Reset all disposable local database and world state with:

```powershell
docker compose -f server/compose.yaml down --volumes
docker compose -f server/compose.yaml up --detach
```

For server validation and playerbot gameplay commands, see
[`docs/testing.md`](docs/testing.md).

## Upstream Updates

Configure the upstream remotes once after cloning:

```powershell
git remote add angelion-upstream https://github.com/Giorox/Angelion-TFS-1.5-Downgrade-8.6.git
git remote add redemption-upstream https://github.com/opentibiabr/otclient.git
```

Import updates as squashed subtree pulls:

```powershell
git subtree pull --prefix=server angelion-upstream 8.60 --squash
git subtree pull --prefix=client redemption-upstream main --squash
```

Review and test each upstream update before merging it into `master`.

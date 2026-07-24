# Tibia Playerbots Project

Custom Tibia 8.60 development base combining:

- `server/`: Giorox Angelion real-map server on the TFS 1.5 downgrade.
- `client/`: current OTClient Redemption configured for standard protocol 8.60.

The repositories are imported as squashed Git subtrees so server and client
changes can be committed atomically while retaining an upstream update path.

## Prerequisites

- Docker Desktop with Docker Compose
- PowerShell 7+
- GitHub CLI authenticated with `gh auth login` (the repository is private)

## Client bootstrap

The executable and Tibia DAT/SPR files are intentionally excluded from Git.
Download and verify the pinned files after cloning:

```powershell
pwsh -File scripts/bootstrap-client.ps1
```

Launch the client with `client/launch-angelion-redemption.cmd`.

## Server

```powershell
docker compose -f server/compose.yaml up --build --detach
docker compose -f server/compose.yaml logs --follow server
```

Local accounts:

| Account | Password | Character |
| ------- | -------- | --------- |
| `admin` | `admin` | `GOD Admin`, `Rook Tester` |
| `bot-one` | `bot-one` | `Bot One` (server-controlled) |

The services bind only to `127.0.0.1`. Reset the disposable database with:

```powershell
docker compose -f server/compose.yaml down --volumes
docker compose -f server/compose.yaml up --detach
```

## Playerbot prototype

The server starts one database-backed, server-controlled `Player` named
`Bot One`. It has no client connection or external bot API. Human clients
cannot take control of the character while the server owns it.

`playerbot-setup` runs before the server on both fresh and existing database
volumes. It reserves the local-development account `bot-one` and character
name `Bot One`, records the character in `player_bots`, and seeds a Carlin
sword and studded shield without replacing existing equipment. Provisioning
fails rather than taking over a same-named or deleted character. Diagnose it
with:

```powershell
docker compose -f server/compose.yaml logs playerbot-setup server
```

The current implementation is a fixed Rookgaard sewer demonstration, not a
general-purpose or configurable bot system. From the seeded position at
`(32097, 32219, 7)`, it navigates to `(32099, 32211, 7)`, uses sewer grate item
`430` at `(32097, 32205, 7)`, searches for visible creatures named `Rat`, and
fights or explores until it dies, exhausts the reachable frontier, or reaches
a bounded failure condition.

Normal shutdown saves the bot through the existing player persistence path.
Controller routes, targets, and explored frontiers remain in memory only. On
restart, a saved position on floor 8 resumes sewer behavior; a saved position
on floor 7 restarts the route to the sewer. Other floors stop the prototype
controller. Removing the Compose volume resets the bot and all other local
world state.

Verify the prototype with:

```powershell
docker compose -f server/compose.yaml config --quiet
docker compose -f server/compose.yaml up --build --detach
docker compose -f server/compose.yaml logs --follow playerbot-setup server
```

Confirm MariaDB is healthy, `playerbot-setup` succeeds, the map and server come
online, and logs report `Playerbot online` followed by sewer and combat events.
Log in as `Rook Tester` with `admin` / `admin` to observe the bot and smoke-test
normal client movement, inventory, item use, combat, and logout. Ports 7171 and
7172 remain bound to localhost.

## Upstream updates

Configure the upstream remotes once after cloning:

```powershell
git remote add angelion-upstream https://github.com/Giorox/Angelion-TFS-1.5-Downgrade-8.6.git
git remote add redemption-upstream https://github.com/opentibiabr/otclient.git
```

```powershell
git subtree pull --prefix=server angelion-upstream 8.60 --squash
git subtree pull --prefix=client redemption-upstream main --squash
```

Review and test each upstream update before merging it into `master`.

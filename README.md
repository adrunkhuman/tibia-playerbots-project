# tibia-playerbots

A Tibia 8.60 real-map server built toward a population made mostly of autonomous players. The aim is a persistent world where bots use the same game mechanics as humans rather than a separate simulation. Bots run inside the server as database-backed `Player` objects: no graphical client, network connection, external bot API, renderer, or dedicated thread is required for each bot.

![Bot One fighting a troll, with recent banking, equipment, and hunting activity in the log](https://github.com/user-attachments/assets/2f1ccafa-af8e-4f7e-8bf7-34edf0a9811b)

## Current state

The development stack controls one seeded character, `Bot One`, a level 8 Knight in Carlin. It can choose map-derived hunting regions, navigate, fight, heal, eat, loot owned corpses, use depots, sell loot, buy supplies and equipment, bank money, learn and cast supported spells, claim supported container rewards, recover after death, and continue from persisted player state.

This is a prototype, not a simulated population. Navigation is bounded rather than a complete whole-map router, quest support is narrow, and most complex behavior is proven by focused fixtures rather than a long-running progression test. Multiple bots, personalities, relationships, parties, guilds, and generated population behavior are planned rather than implemented. See [Playerbots](docs/playerbots.md) for the capability and evidence boundaries.

## Run locally

Requirements:

- PowerShell 7+ (`pwsh`)
- Docker Desktop on Windows, or Docker Engine with Compose v2 on Linux
- Windows: GitHub CLI authentication when the pinned private client executable must be downloaded
- Linux: a locally built `client/otclient`

From the repository root:

```powershell
pwsh -File scripts/bootstrap-client.ps1
docker compose -f server/compose.yaml up --build --detach
docker compose -f server/compose.yaml logs --follow server
```

Launch `client/launch-angelion-redemption.cmd` on Windows. On Linux, run `(cd client && ./otclient)` from a POSIX shell. The server listens only on `127.0.0.1:7171` and `127.0.0.1:7172`.

| Account | Password | Characters |
| --- | --- | --- |
| `admin` | `admin` | `GOD Admin`, `Rook Tester` |
| `bot-one` | `bot-one` | `Bot One` |

A human client cannot control `Bot One` while the server owns it. Local database and world state are disposable. This command deletes them:

```powershell
docker compose -f server/compose.yaml down --volumes
```

## Development

| Task | Documentation |
| --- | --- |
| Understand behavior and limits | [Playerbots](docs/playerbots.md) |
| Choose and run checks | [Testing](docs/testing.md) |
| Install or update the client runtime | [Client runtime](docs/client-runtime.md) |
| Contribute changes | [Contributing](CONTRIBUTING.md) |
| Follow repository maintenance rules | [AGENTS.md](AGENTS.md) |

Current plans and temporary findings belong in [GitHub issues](https://github.com/adrunkhuman/tibia-playerbots-project/issues). Configuration and code own tunable values; pull requests and Git history own implementation history.

## Upstreams

The repository integrates [Giorox Angelion TFS 1.5 downgrade](https://github.com/Giorox/Angelion-TFS-1.5-Downgrade-8.6) branch `8.60` under `server/` and [OTClient Redemption](https://github.com/opentibiabr/otclient) branch `main` under `client/`. Both are squashed Git subtrees, not submodules. Pinned baselines are recorded in `.subtree/`; review and test upstream updates before merging them into `master`.

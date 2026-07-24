# Project Guidance

## Scope

This repository is the integrated Angelion 8.60 server and OTClient Redemption
development base. `master` is the authoritative branch. The verified starting
point is tagged `angelion-base-v1.1`.

The repository is a monorepo with two squashed Git subtrees:

- `server/`: Giorox Angelion TFS 1.5 downgrade, upstream branch `8.60`.
- `client/`: OpenTibiaBR OTClient Redemption, upstream branch `main`.

The narrower rules in `client/AGENTS.md` also apply to work under `client/`.

## Git And Upstreams

- Do not create nested Git repositories or convert the subtrees to submodules.
- Keep server and client changes in the same commit when they implement one
  cross-stack behavior or protocol change.
- Import upstream updates with squashed subtree pulls:

```powershell
git subtree pull --prefix=server angelion-upstream 8.60 --squash
git subtree pull --prefix=client redemption-upstream main --squash
```

- Review and test upstream updates before merging them into `master`.
- Prefer intent-oriented commit scopes such as `server:`, `client:`, `protocol:`,
  `content:`, `balance:`, `infra:`, and `upstream:`.
- Treat tracked configuration that changes gameplay as product behavior, not
  disposable local configuration.

## Protocol And Client

- The compatibility target is standard Tibia protocol 8.60.
- Do not enable Mateuzkl-specific or other custom packet extensions unless the
  matching server implementation is added and both sides are tested together.
- Keep client asset auto-installation disabled for this project. Runtime assets
  are installed by `scripts/bootstrap-client.ps1`.
- Preserve the standard 8.60 feature boundaries in
  `client/modules/game_features/features.lua`.
- Preserve these verified local defaults unless intentionally changing game
  behavior:
  - `force-new-walking-formula: true`
  - `item-ticks-per-frame: 500`
  - right-click NPC talk enabled
  - server endpoint `127.0.0.1:7171`, protocol `860`
- Preserve the isolated client profile name `angelion-redemption`.

## Runtime Assets

- Never commit `Tibia.dat`, `Tibia.spr`, OTClient executables, logs, screenshots,
  minimap caches, or database volumes.
- Restore the pinned client runtime and assets with:

```powershell
pwsh -File scripts/bootstrap-client.ps1
```

- Do not weaken or remove SHA-256 verification in the bootstrap script.
- Update the durable private release asset, expected hashes, and documentation
  together when changing the client executable or Tibia assets.
- `server/data/world/World.otbm` is close to GitHub's 100 MiB file limit. Check
  its size before committing map growth; introduce Git LFS before it crosses
  that limit.

## Server And Database

- Docker Compose is the supported local server environment.
- Keep the Compose project name `angelion` so resources do not depend on the
  checkout directory name.
- Keep ports `7171` and `7172` bound to `127.0.0.1` unless external exposure is
  explicitly requested and the deployment is hardened first.
- The local database and world state are disposable. Schema and development
  accounts must be reproducible from the ordered SQL files mounted in
  `server/compose.yaml`.
- During the current development phase, do not preserve database or world state
  at the cost of a simpler reset, rebuild, or restart. Use a clean volume when
  useful unless the task explicitly requires persistence testing.
- Preserve the seeded development characters after database resets:
  `GOD Admin`, `Rook Tester`, and the server-controlled `Bot One`.
- Keep `playerbot-setup` ahead of the server in the Compose dependency chain.
  It owns idempotent bot provisioning and the `player_bots` registry, and must
  fail rather than take over an unrelated same-named or deleted character.
- Do not commit real credentials or production secrets. The tracked credentials
  are local development defaults only.
- Preserve the CRLF normalization in `server/src/rsa.cpp`; Windows checkouts
  otherwise fail to load the PEM key.
- Do not restore global `-Werror` in the legacy server build without first
  resolving and validating all compiler warnings on the supported toolchain.

## Playerbots

- `Bot One` is the current database-backed, server-controlled `Player`. It has
  no client connection or external bot API, and a human client must not take
  control while the server owns it.
- Preserve normal player persistence for bots. A clean shutdown must save them
  through the existing player save path rather than a parallel persistence
  mechanism.
- The hardcoded Rookgaard sewer route and combat behavior are a bounded
  prototype, not the settled bot architecture or product goal. Discuss and
  document broader goals before generalizing the prototype.
- The intended scale is eventually hundreds of bots. Do not introduce one OS
  thread, graphical client, renderer, UI, or blocking loop per bot.
- Schedule bot decisions through the server dispatcher/scheduler, stagger work,
  and use normal movement, combat, spell, and item APIs instead of directly
  mutating world or inventory state.

## Continuous Integration

- Keep `.github/workflows/server-ci.yml` green for server and Compose changes.
  It validates a fresh disposable stack, database health, playerbot
  provisioning, server startup, and both local game ports.
- The workflow intentionally runs only for `server/**` and workflow changes.
  Documentation-only changes outside those paths do not require a server build.
- The workflow depends on the stable Compose project and service names. Update
  its container assertions together with any Compose rename.

## Verification

For server, infrastructure, or cross-stack changes, run at minimum:

```powershell
pwsh -File scripts/bootstrap-client.ps1
docker compose -f server/compose.yaml config --quiet
docker compose -f server/compose.yaml up --build --detach
docker compose -f server/compose.yaml logs playerbot-setup server
```

Confirm that MariaDB is healthy, the map loads, the server reports online, and
ports `7171` and `7172` accept local connections. Confirm that
`playerbot-setup` exits successfully, exactly one valid `Bot One` registration
exists, and server logs report `Playerbot online`.

For protocol or gameplay-facing client changes, also test:

- Login and character selection
- Movement and tile updates
- Containers and inventory
- Books and writable items
- NPC conversation and trade
- Use-with actions
- Combat and death
- Logout and persistence

Do not claim client compatibility based only on a successful login. Watch both
client and server logs for parser errors, unknown opcodes, restart loops, and
runaway memory use.

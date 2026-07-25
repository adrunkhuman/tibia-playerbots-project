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
name `Bot One`, records the character in `player_bots`, and inserts a backpack,
Carlin sword, and studded shield only when their respective equipment slots are
empty. Existing slot contents are never replaced; provisioning does not repair
an occupied slot. It fails rather than taking over a same-named or deleted
character. Diagnose it with:

```powershell
docker compose -f server/compose.yaml logs playerbot-setup server
```

Playerbot events are JSON Lines on server stdout. Retrieve records for AI or
scripted analysis without the Compose prefix with:

```powershell
docker compose -f server/compose.yaml logs --no-log-prefix --since 30m server | Where-Object { $_ -match '"component":"playerbot"' }
```

Every record has `schema: 1`, a UTC RFC 3339 `ts`,
`component: "playerbot"`, `event`, `bot`, persistent `player_id`, and
`position`. Event types are `lifecycle`, `state_transition`, `target_changed`,
`action_result`, `stuck`, `summary`, and `terminal`. Event-specific fields are
conditional, and `target_id` is `null` when no target exists.

| Event | Distinguishing fields |
| ----- | --------------------- |
| `lifecycle` | `status`: `online`, `dead`, or `removed` |
| `state_transition` | `from`, `to` |
| `target_changed` | `previous_target_id`, `target_id`, optional target type and position, `reason` |
| `action_result` | Always has `action` and `result`; failures also have `reason`. Successful `loot` records have `item_id`, `count`, and post-action `inventory_count`. Successful `eat` records additionally have `food_ticks`, the post-action regeneration duration in milliseconds. |
| `stuck` | `reason`, `blocked_steps` |
| `summary` | current state and target, uptime, decision/pathfinding timing, action, failure, stuck, and suppression counters |
| `terminal` | `reason` |

States, actions, results, statuses, and reasons are stable lowercase strings
intended for machine consumption.

The controller logs lifecycle and state changes, target changes, failures,
stuck detection, terminal reasons, and a cumulative summary every 60 seconds.
Successful movement does not produce one record per tile. Repeated identical
transitions, target changes, and action failures are emitted at most once per
60 seconds; `summary.suppressed_events` counts omitted repetitions. Server
container logs use Docker's local driver with three 10 MiB files, retaining
roughly 30 MiB; older records are discarded. Summary `uptime_ms` is in
milliseconds, timing fields ending in `_us` are in microseconds, and counters
cover one in-memory controller lifetime. They reset when the server or
controller restarts.

The current implementation is a fixed Rookgaard sewer demonstration, not a
general-purpose or configurable bot system. From the seeded position at
`(32097, 32219, 7)`, it navigates to `(32099, 32211, 7)`, uses sewer grate item
`430` at `(32097, 32205, 7)`, searches for visible creatures named `Rat`, and
fights or explores until it dies, exhausts the reachable frontier, or reaches
a bounded failure condition. After a kill, it attempts to find an owned rat
corpse within one tile, open it through the normal player action path, and move
available gold plus up to three total pieces of cheese into the backpack.
Corpse ownership, range, capacity, container space, item stacking, action
delays, and bounded corpse-search/open retries remain enforced. If the corpse
or an item cannot be accessed, the controller records a failed `loot` action
and resumes hunting; it does not guarantee that every drop is transferred.
When it can eat another cheese, it consumes one through the normal item-use
path, verifies both the inventory decrease and food-condition increase, and
repeats until another cheese would exceed the game's fullness limit or it runs
out of cheese.

Normal shutdown saves the bot through the existing player persistence path.
Controller routes, targets, and explored frontiers remain in memory only. On
restart, a saved position on floor 8 resumes sewer behavior; a saved position
on floor 7 restarts the route to the sewer. Other floors stop the prototype
controller. Removing the Compose volume resets the bot and all other local
world state.

To inspect Bot One through the client, first stop the server cleanly so its
inventory is saved, then restart without the server-controlled bot:

```powershell
docker compose -f server/compose.yaml stop server
$env:PLAYERBOT_ENABLED = "false"
docker compose -f server/compose.yaml up --detach --force-recreate server
```

Log in with `bot-one` / `bot-one`. Log the character out before restoring the
normal startup mode with `Remove-Item Env:PLAYERBOT_ENABLED` and recreating the
server. Live ownership transfer remains intentionally unsupported.

`PLAYERBOT_ENABLED` is evaluated when the server starts. The exact value
`"false"` disables controller startup; an unset variable or any other value
enables it. Disabling the controller does not remove `Bot One` or skip
`playerbot-setup`; the character remains reserved and can be inspected through
the client. Recreate the server after changing the variable.

Server CI mounts `server/tests/playerbot_connectionless.lua` through
`server/compose.playerbot-regression.yaml`; the normal Compose stack never
loads it. The probe covers representative Lua UI and network sends, temporary
inventory/container mutation, death, explicit removal, rejected login, and
clean-shutdown persistence for the connectionless bot.

Run the connectionless interaction probe locally with the regression overlay:

```powershell
$env:PLAYERBOT_REGRESSION_MODE = "interactions"
docker compose -f server/compose.yaml -f server/compose.playerbot-regression.yaml up --build --detach --force-recreate server
```

After the interaction mode reaches its food-consumption assertion, stop the
server cleanly before recreating it with mode `death`; that mode verifies the
saved food condition and remaining cheese before killing the bot. Modes
`remove` and `reject` cover explicit removal and rejected login. Remove
`PLAYERBOT_REGRESSION_MODE` and the regression overlay before returning to the
normal stack.

Verify the prototype with:

```powershell
docker compose -f server/compose.yaml config --quiet
docker compose -f server/compose.yaml up --build --detach
docker compose -f server/compose.yaml logs --follow playerbot-setup server
```

Confirm MariaDB is healthy, `playerbot-setup` succeeds, the map and server come
online, and JSONL logs report a `lifecycle` event with status `online`.
`state_transition` and `target_changed` events follow when the scenario
progresses; failure, stuck, summary, and terminal events occur only under their
respective conditions.
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

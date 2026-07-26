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

`Rook Tester` is a reproducible level-50 traversal test character with sword
and shielding skills, a backpack, plate armor, dark shield, fire sword, Boots
of Haste, rope, and shovel. The loadout is recreated with every fresh database.

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
name `Bot One`, records the character in `player_bots`, and provisions the same
level-50 traversal loadout as `Rook Tester`. Equipment is inserted only when
its slot is empty. Existing slot contents are never replaced; provisioning does
not repair an occupied slot. It fails rather than taking over a same-named or
deleted character. Diagnose it with:

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
`position`. Event types are `lifecycle`, `state_transition`,
`objective_transition`, `target_changed`, `action_result`, `stuck`, `summary`,
and `terminal`. Event-specific fields are conditional, and `target_id` is
`null` when no target exists.

| Event | Distinguishing fields |
| ----- | --------------------- |
| `lifecycle` | `status`: `online`, `dead`, or `removed` |
| `state_transition` | `from`, `to` |
| `objective_transition` | Cycle phase `from`, `to`, and `reason`; phases are `return_to_depot`, `deposit_loot`, and `hunt` |
| `target_changed` | `previous_target_id`, `target_id`, optional target type and position, `reason` |
| `action_result` | Always has `action` and `result`; failures and skipped actions also have `reason`. Navigation plans include their destination, step count, and expanded-node count. Successful `eat`, `loot`, and `deposit` records include item details. A corpse found empty immediately after normal opening emits `action=loot`, `result=skipped`, `reason=corpse_empty`, `corpse_item_id`, `corpse_owner_id`, and `corpse_position`. |
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

The current implementation is a bounded Rookgaard hunt/depot demonstration,
not a general-purpose bot system. It plans multi-floor routes from map state
instead of consuming an ordered checkpoint route. Planning is limited to a
192-tile margin around the endpoints and 100,000 expanded nodes. The supported
transition adapters cover ordinary floor changes, configured ladder, rope,
shovel, and direct-use holes, simple teleports, and unlocked doors. This is not
yet arbitrary whole-map navigation or a datapack-derived transition registry.

Walking executes with the player's normal server walk and action delays. Route
selection weights cardinal steps at 10 and diagonal steps at 30, reflecting the
server's diagonal penalty. A failed step is excluded for 10 seconds, allowing a
temporary player or creature blockage to cause a detour or bounded wait.
Persistent missing routes stop the controller after bounded retries. Missing
depot state, rejected deposits, and capacity that remains below the return
threshold after depositing are also terminal failures.

After depositing carried loot on the tile south of `(32105, 32195, 8)`, the bot
hunts along `(32084, 32144, 5)`, `(32103, 32124, 8)`,
`(32117, 32090, 9)`, and back through the middle point. It returns after five
minutes or when free capacity falls below 20 oz, stops attack/follow behavior,
and deposits carried loot. Equipped items and the root backpack are not
examined. Top-level backpack items, including loot containers as complete
units, are moved to the world tile directly south of the standing position;
rope `2120`, shovel `2554`, and cheese are retained. This fake depot is not
private or durable depot storage: other players can move its contents, and map
tile contents are not preserved across a clean world reset.

`PLAYERBOT_HUNT_DURATION_SECONDS` changes the hunt window at server startup and
defaults to `300`. Invalid text falls back to `300`; zero and negative values
are clamped to one second. Recreate the server container after changing it.

The bot retains the verified food behavior: when it can eat another cheese, it
consumes one through the normal item-use path, verifies both the inventory
decrease and food-condition increase, and repeats until another cheese would
exceed the game's fullness limit or it runs out of cheese.

Corpse discovery does not inspect contents. It accepts only server-identified
corpse containers; owner `0` is unrestricted, while an owned corpse requires
`Player::canOpenCorpse`, which allows the owner or an eligible party member.
The bot then approaches and opens the corpse through normal item use before
checking whether it is empty. Ineligible corpses are ignored during bounded
search and can end as `owned_corpse_unavailable`; repeated normal-use failures
end as `corpse_open_failed`.

Normal shutdown saves the bot through the existing player persistence path.
Routes, objectives, hunt deadlines, targets, action attempts, transient blocked
positions, and combat suppression remain in memory only. Startup conservatively
returns the bot from its actual persisted position to the fake depot before
starting another cycle. Removing the Compose volume resets the bot and all
other local world state.

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

The smoke workflow runs only the normal Compose stack and checks fresh
provisioning, server startup, structured lifecycle output, and local ports. It
does not execute gameplay actions. The optional manual regression overlay
mounts `server/tests/playerbot_connectionless.lua` and covers representative
Lua UI and network sends, temporary inventory/container mutation, death,
explicit removal, rejected login, and clean-shutdown persistence.

The local-only gameplay suite injects nested loot, verifies the fake-depot drop
and protected equipment/tools, triggers a shortened deadline return, and
requires a second hunt cycle:

```powershell
pwsh -File scripts/test-playerbot-gameplay.ps1
```

`-FullNavigation` additionally verifies the complete A-B-C-B-A coordinate
sequence and temporary-blockage recovery with ambient monsters suppressed by
the test overlay. It is an end-to-end route exercise, not an independent test
matrix for every supported transition adapter:

```powershell
pwsh -File scripts/test-playerbot-gameplay.ps1 -FullNavigation
```

`-CorpseLoot` first runs the baseline cycle test, resets the disposable stack,
then creates deterministic empty and guaranteed-loot monsters. It verifies that
the bot identifies an openable corpse from server item/ownership metadata,
opens it normally before classifying it as empty, and preserves the normal
item-move path for a corpse containing gold:

```powershell
pwsh -File scripts/test-playerbot-gameplay.ps1 -CorpseLoot
```

The suite rebuilds the server and resets the disposable Compose stack and
database volume. It removes them afterward; `-KeepStack` leaves the final stack
running. The default timeout is 300 seconds and can be changed with
`-TimeoutSeconds` (60-3600).

This prototype advances the mainland loop tracked in #22 and the navigation
architecture tracked in #28 without closing either umbrella issue. Empty
corpse classification implements the first slice of #29; moved, expired,
inaccessible, non-owned, and policy-rejected corpse outcomes remain follow-up.

Run the connectionless interaction probe locally with the regression overlay:

```powershell
$env:PLAYERBOT_REGRESSION_MODE = "interactions"
docker compose -f server/compose.yaml -f server/compose.playerbot-regression.yaml up --build --detach --force-recreate server
```

After observing the desired interaction or traversal behavior, stop the server
cleanly before recreating it with mode `death`; that mode verifies the saved
food condition and inventory state before killing the bot. Modes
`remove` and `reject` cover explicit removal and rejected login. Remove
`PLAYERBOT_REGRESSION_MODE` and the regression overlay before returning to the
normal stack.

Run each follow-up mode separately, waiting for its `PASS` marker before
stopping or recreating the server:

```powershell
docker compose -f server/compose.yaml -f server/compose.playerbot-regression.yaml stop server
$env:PLAYERBOT_REGRESSION_MODE = "death"
docker compose -f server/compose.yaml -f server/compose.playerbot-regression.yaml up --detach --force-recreate server

docker compose -f server/compose.yaml -f server/compose.playerbot-regression.yaml stop server
$env:PLAYERBOT_REGRESSION_MODE = "remove"
docker compose -f server/compose.yaml -f server/compose.playerbot-regression.yaml up --detach --force-recreate server

docker compose -f server/compose.yaml -f server/compose.playerbot-regression.yaml stop server
$env:PLAYERBOT_REGRESSION_MODE = "reject"
docker compose -f server/compose.yaml -f server/compose.playerbot-regression.yaml up --detach --force-recreate server
```

Return to the normal stack with:

```powershell
docker compose -f server/compose.yaml -f server/compose.playerbot-regression.yaml stop server
Remove-Item Env:PLAYERBOT_REGRESSION_MODE
docker compose -f server/compose.yaml up --detach --force-recreate server
```

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

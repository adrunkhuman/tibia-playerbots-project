# Playerbots

## Current Model

The server starts one database-backed, server-controlled `Player` named
`Bot One`. It has no client connection or external bot API. Human clients
cannot take control of the character while the server owns it.

Bot-facing UI and network notifications must remain behind null-safe `Player`
methods. Playerbot code and datapack scripts must not dereference `client`.
Normal shutdown saves bots through the existing player persistence path rather
than a parallel persistence mechanism.

The intended scale is eventually hundreds of bots. Decisions run through the
server dispatcher and scheduler; the design must not add one OS thread,
graphical client, renderer, UI, or blocking loop per bot.

## Provisioning

`playerbot-setup` runs before the server on fresh and existing database volumes.
It reserves the local-development account `bot-one` and character name
`Bot One`, records the character in `player_bots`, and provisions the same
level-50 traversal loadout as `Rook Tester`.

Equipment is inserted only when its slot is empty. Existing slot contents are
never replaced, and provisioning does not repair an occupied slot. Setup fails
rather than taking over an unrelated same-named or deleted character.

Diagnose provisioning and startup with:

```powershell
docker compose -f server/compose.yaml logs playerbot-setup server
```

## Structured Logs

Playerbot events are JSON Lines on server stdout. Retrieve records without the
Compose prefix with:

```powershell
docker compose -f server/compose.yaml logs --no-log-prefix --since 30m server | Where-Object { $_ -match '"component":"playerbot"' }
```

Every record has `schema: 1`, a UTC RFC 3339 `ts`,
`component: "playerbot"`, `event`, `bot`, persistent `player_id`, and
`position`. Event-specific fields are conditional, and `target_id` is `null`
when no target exists.

| Event | Distinguishing fields |
| ----- | --------------------- |
| `lifecycle` | `status`: `online`, `dead`, or `removed` |
| `state_transition` | `from`, `to` |
| `objective_transition` | Cycle phase `from`, `to`, and `reason`; phases are `return_to_depot`, `deposit_loot`, and `hunt` |
| `target_changed` | `previous_target_id`, `target_id`, optional target type and position, `reason` |
| `action_result` | Always has `action` and `result`; failures and skipped actions also have `reason`. Navigation plans include destination, step count, and expanded-node count. Successful `eat`, `loot`, and `deposit` records include item details. |
| `stuck` | `reason`, `blocked_steps` |
| `summary` | Current state and target, uptime, decision/pathfinding timing, action, failure, stuck, and suppression counters |
| `terminal` | `reason` |

A corpse found empty immediately after normal opening emits `action=loot`,
`result=skipped`, `reason=corpse_empty`, `corpse_item_id`, `corpse_owner_id`,
and `corpse_position`.

States, actions, results, statuses, and reasons are stable lowercase strings for
machine consumption. The controller emits a cumulative summary every 60
seconds. Successful movement does not produce one record per tile. Repeated
identical transitions, target changes, and action failures are emitted at most
once per 60 seconds; `summary.suppressed_events` counts omitted repetitions.

Server container logs use Docker's local driver with three 10 MiB files,
retaining roughly 30 MiB. Summary `uptime_ms` is in milliseconds, fields ending
in `_us` are in microseconds, and counters cover one in-memory controller
lifetime. They reset when the server or controller restarts.

## Hunt And Depot Loop

The current implementation is a bounded Rookgaard demonstration, not a
general-purpose bot system or settled whole-map navigation. Callers provide
destination goals rather than ordered transition checkpoints.

The navigator searches loaded map state within a 192-tile margin around the
endpoints and a 100,000-node expansion cap. Transition adapters cover ordinary
floor changes, configured ladder, rope, shovel and direct-use holes, simple
teleports, and unlocked doors. This is not yet a datapack-derived transition
registry.

Walking uses normal server movement and action delays. Route selection weights
cardinal steps at 10 and diagonal steps at 30. A failed step is excluded for 10
seconds so a temporary creature blockage can cause a detour or bounded wait.
Persistent missing routes stop the controller after bounded retries.

After depositing carried loot on the tile south of `(32105, 32195, 8)`, the bot
hunts along `(32084, 32144, 5)`, `(32103, 32124, 8)`,
`(32117, 32090, 9)`, and back through the middle point. It returns after five
minutes or when free capacity falls below 20 oz and cancels attack/follow
behavior before returning.

The fake depot is a public world tile, not private or durable depot storage.
Other players can move its contents, and map tile contents are not preserved
across a clean world reset. Equipped items and the root backpack are not
examined. Top-level backpack items, including loot containers as complete
units, are deposited. Rope `2120`, shovel `2554`, and cheese are retained.
Missing depot state, rejected deposits, and capacity that remains below the
threshold after depositing are terminal failures.

## Food And Corpses

The bot eats cheese through normal item use, verifies both the inventory
decrease and food-condition increase, and stops before another cheese would
exceed the game's fullness limit or it runs out of cheese.

Corpse discovery does not inspect contents. It accepts only server-identified
corpse containers; owner `0` is unrestricted, while an owned corpse requires
`Player::canOpenCorpse`, which permits the owner or an eligible party member.
The bot approaches and opens the corpse through normal item use before checking
contents. It prefers the newest corpse when several occupy one tile and retains
the selected opened container throughout looting.

Ineligible corpses are ignored during bounded search and can end as
`owned_corpse_unavailable`; repeated normal-use failures end as
`corpse_open_failed`. Moved, expired, inaccessible, non-owned, and
policy-rejected corpse outcomes remain future work in issue #29.

## Lifecycle And Configuration

Routes, objectives, hunt deadlines, targets, action attempts, transient blocked
positions, and combat suppression remain in memory only. Startup conservatively
returns the bot from its actual persisted position to the fake depot before
starting a fresh cycle. Removing the Compose volume resets the bot and all
other local world state.

`PLAYERBOT_HUNT_DURATION_SECONDS` changes the hunt window at server startup and
defaults to `300`. Invalid text falls back to `300`; zero and negative values
are clamped to one second. Recreate the server container after changing it.

`PLAYERBOT_ENABLED` is also evaluated at startup. The exact value `"false"`
disables controller startup; an unset variable or any other value enables it.
Disabling the controller does not remove `Bot One` or skip `playerbot-setup`.

## Human Inspection

Stop the server cleanly so Bot One is saved, then recreate it without the
server-controlled bot:

```powershell
docker compose -f server/compose.yaml stop server
$env:PLAYERBOT_ENABLED = "false"
docker compose -f server/compose.yaml up --detach --force-recreate server
```

Log in with `bot-one` / `bot-one`. Log the character out before restoring normal
startup with `Remove-Item Env:PLAYERBOT_ENABLED` and recreating the server. Live
ownership transfer is intentionally unsupported.

## Scope

The prototype advances the mainland loop tracked in issue #22 and navigation
architecture tracked in issue #28 without closing either umbrella. See
[`testing.md`](testing.md) for validation commands.

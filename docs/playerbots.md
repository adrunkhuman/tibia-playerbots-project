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
level-50 traversal loadout as `Rook Tester`. Each character receives its
10,000 gp bank balance only when first created. Bot One also receives two
100-gp backpack stacks only when first created; rerunning setup never restores
spent balances or coins. Global `freePremium = true` supplies local premium
access, so provisioning does not create account-specific premium expiries.

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
| `objective_transition` | Cycle phase `from`, `to`, and `reason`; phases are `service`, `return_to_depot`, `deposit_loot`, and `hunt` |
| `service_discovered` | Tagged live NPC name, capability, ID, and registered offer count |
| `npc_reply` | Selected NPC name, ID, and private acknowledgement text |
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
registry. The datapack classifies some wall windows as doors; items whose
lowercase name contains `window` are intentionally excluded from door
transitions and treated as obstacles. This name-based exception is
case-sensitive.

Walking uses normal server movement and action delays. Route selection weights
cardinal steps at 10 and diagonal steps at 30. A failed step is excluded for 10
seconds so a temporary creature blockage can cause a detour or bounded wait.
Persistent missing routes stop the controller after bounded retries.

At startup and after the five-minute deadline or 30 oz capacity threshold, the
bot discovers live NPCs whose XML has an exact `playerbot_service` value of
`shop` or `banker`; other or missing values are ignored. For example:

```xml
<parameter key="playerbot_service" value="shop" />
```

Each tagged shop's `ShopModule` registers its loaded XML buy/sell offers on the
owning live NPC. A tagged shop without registered offers is ignored. The bot
uses that catalog to select the nearest live merchant that provides its needed
sale or purchase before moving, rather than visiting every shop to probe its
window. The selected merchant is still greeted and its normal trade window is
opened for each transaction.

The bot sends `hi` through the selected NPC's normal speech handler and treats
any private reply from that same NPC as focus acknowledgement; reply text is
not interpreted. It then sends `trade` and requires that NPC to own the loaded
shop window before transacting. Greeting and shop-window setup each permit
three attempts; exhausted focus or window attempts emit
`npc_focus_unconfirmed` or `shop_window_unavailable` and stop the controller.

The current sell allowlist is dead rat (`2813`), bear paw (`5896`), wolf paw
(`5897`), and minotaur leather (`5878`). Other loot is not sold; top-level
items and nested bags are handled by the fake-depot policy below. The bot buys
exactly any deficit to five small health potions (`8704`) and one meat (`2666`),
then uses the nearest matching live banker's dialogue to deposit all carried
money and withdraw 100 gp. NPC names and coordinates are telemetry, not
controller inputs.

After depositing remaining carried loot on the tile south of `(32105, 32195, 8)`, the bot
hunts along `(32084, 32144, 5)`, `(32103, 32124, 8)`,
`(32117, 32090, 9)`, and back through the middle point. It returns after five
minutes or when free capacity falls below 30 oz and cancels attack/follow
behavior before returning.

The fake depot is a public world tile, not private or durable depot storage.
Other players can move its contents, and map tile contents are not preserved
across a clean world reset. Equipped items and the root backpack are not
examined. Top-level backpack items, including loot containers as complete
units, are deposited. Equipped items and the root backpack remain untouched;
rope `2120`, shovel `2554`, potion reserve `8704`, meat reserve `2666`, and
carried currency are retained.
Missing depot state, rejected deposits, and capacity that remains below the
threshold after depositing are terminal failures.

## Food And Corpses

The bot eats surplus meat through normal item use, verifies both the inventory
decrease and food-condition increase, and stops before another meat would
exceed the game's fullness limit or consume its one-meat reserve.

Corpse discovery does not inspect contents. It accepts only server-identified
corpse containers; owner `0` is unrestricted, while an owned corpse requires
`Player::canOpenCorpse`, which permits the owner or an eligible party member.
The bot approaches and opens the corpse through normal item use before checking
contents. It prefers the newest corpse when several occupy one tile and retains
the selected opened container throughout looting.

Opening an eligible but empty corpse emits `corpse_empty` as a skipped result,
not an action failure. A bounded search that finds no eligible corpse emits
`owned_corpse_unavailable`; the name also currently covers stale, moved, or
otherwise missing corpses. Repeated normal-use failures emit
`corpse_open_failed`, a missing root backpack emits `backpack_unavailable`, and
an unverified corpse-to-backpack move emits `item_move_failed`. The latter does
not yet distinguish full slots, insufficient capacity, or another rejected
move. Finer moved, expired, inaccessible, non-owned, and policy-rejected corpse
outcomes remain future work in issue #29.

## Lifecycle And Configuration

Routes, objectives, hunt deadlines, service catalogs, targets, action attempts,
transient blocked positions, and combat suppression remain in memory only.
Startup begins a fresh service cycle from the bot's actual persisted position.
Removing the Compose volume resets the bot and all other local world state.

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

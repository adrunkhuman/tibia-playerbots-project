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
`Bot One`, records the character in `player_bots`, and provisions it at level 1
with normal level-1 health, mana, and capacity, a starter jacket and club, and
the existing high sword and shielding skills used by the hunt prototype. It
also receives a backpack, rope, and shovel. Each character receives its
100 gp bank balance only when first created. Bot One also receives two
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
| `lifecycle` | `status`: `online`, `dead`, `removed`, `recovery_scheduled`, `recovery_failed`, or `recovery_abandoned`. Death records include level, health, objective, controller state, target, last-hit killer, and most-damage source. Recovery records include death count, relog attempt, delay, and bounded failure reason where applicable. |
| `state_transition` | `from`, `to` |
| `objective_transition` | Objective `from`, `to`, and `reason`; progression currently adds `pickup_reward` above cycle phases `service`, `return_to_depot`, `deposit_loot`, and `hunt` |
| `strategy_candidate` | Pickup-reward candidate ID, item/slot/stat benefit, travel evidence, feasibility, and rejection reason |
| `reward_inspection` | Generic reward UID/destination, recursive item paths, names/counts, classifications, known utility, currency/sell totals, upgrade/container/unknown counts, and the complete known bundle tree flattened with containment paths |
| `strategy_selection` | Selected goal/candidate and the inspectable selection reason |
| `strategy_objective_result` | Goal/candidate, result, and terminal objective reason |
| `goal_candidate` | Decision ID/reason, `service`, `pickup_reward`, or `hunt`, evaluated/feasible flags, utility, and goal-specific evidence |
| `goal_selection` | Decision ID/reason, previous and selected goal, utility, rationale, and optional forced-interrupt marker |
| `goal_result` | Decision ID, completed/interrupted goal, result, and reason |
| `service_discovered` | Tagged live NPC name, capability, ID, and registered offer count |
| `npc_reply` | Selected NPC name, ID, and private acknowledgement text |
| `target_changed` | `previous_target_id`, `target_id`, optional target type and position, `reason` |
| `action_result` | Always has `action` and `result`; failures and skipped actions also have `reason`. Navigation plans include destination, step count, and expanded-node count. Successful `heal`, `eat`, `loot`, and `deposit` records include item details. A successful `claim_reward` includes selected-root counts before and after use, `top_level_root_count`, and `all_roots_verified`. A requested `open_reward_container` includes `container_id`, nested `depth`, and `item_id`; bounded open failure ends the objective with `reward_container_open_failed`. |
| `stuck` | `reason`, `blocked_steps` |
| `summary` | Current state and target, uptime, decision/pathfinding timing, action, failure, stuck, and suppression counters |
| `terminal` | `reason` |
| `hunt_region_candidate` | `region_id`, floor, center/destination, patrol count, expected/observed/projected XP fields, threat, score, route evidence, suitability/reachability, optional rejection reason, and complete monster profiles. Rejections include `predicted_damage`, `observed_danger_cooldown`, `unreachable`, and `reachability_budget`. |
| `hunt_region_scan` | Selection `reason`, player level/health/armor/defense, and `candidate_count` |
| `hunt_region_selection` | `result=selected`, region and reason; or `result=failed`, `reason=no_suitable_reachable_region` |
| `hunt_region_outcome` | Region/reason, duration, before/after level, XP gained/rate/projection/correction, kills, and damage taken |
| `hunt_region_patrol` | `result=skipped`, region/destination, and `reason=unreachable` or `position_oscillation` |
| `navigation_progress` | `result=suppressed`, `reason=position_oscillation`, destination, blocked transition target, and alternating positions |

Recovery lifecycle fields are evidence-specific:

| Status | Reason | Additional fields |
| ------ | ------ | ----------------- |
| `online` | none | `recovered`, `recovery_count`, `objective` |
| `recovery_scheduled` | `death` or `relog_retry` | `death_count`, `relog_attempt`, `delay_ms` |
| `recovery_failed` | `player_removal_failed` | none |
| `recovery_failed` | `relog_failed` | `relog_attempt` |
| `recovery_abandoned` | `death_loop_limit` | `death_count`, `maximum_deaths` |
| `recovery_abandoned` | `ownership_conflict` or `relog_attempt_limit` | none |

A corpse found empty immediately after normal opening emits `action=loot`,
`result=skipped`, `reason=corpse_empty`, `corpse_item_id`, `corpse_owner_id`,
and `corpse_position`.

A healing result includes `method`, `item_id`, `trigger`, `objective`, `state`,
`health_before`, `health_after`, `health_max`, `resource_before`, and
`resource_after`. Verified potion consumption with net health gain is
`result=success`. Missing stock is `result=skipped`, `reason=missing_supply`;
unobserved consumption is `result=failed`, `reason=use_not_verified`; and
observed consumption without net health gain is `result=failed`,
`reason=ineffective_recovery`.

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

Normal startup first evaluates the initial progression slice. It enumerates
loaded unique map items whose action ID is the generic quest-chest handler
`2000`, restricts them to the existing bounded Rookgaard area, and recursively
inspects every top-level reward and nested container item while preserving root
ordinals and child paths. Loaded metadata and current player state classify
known equipment upgrades, currency, configured supplies/food/tools, known NPC
sellables, containers, and safe `unknown_keep` items. Unknown items remain
visible and protected but receive no invented utility. Claimed storage,
recursive bundle weight, top-level root slots, displaced-equipment space, and
bounded navigator reachability are checked before selection. Routes requiring
a door, rope, or shovel action, or more than 120 steps, are not considered
simple pickup objectives.

This progression slice supports only shared container rewards handled by action
ID `2000`. Action ID `2001` uses the separate non-container reward table and is
not inspected or claimed. Dedicated quest actions, including quest levers, are
also outside this slice. Their rewards require separate behavior rather than
container-tree inspection.

Reward bundles are ordered by known utility minus estimated travel, then route
validated until a useful candidate succeeds. Equipment bundles receive the
existing progression priority; purely economic bundles use a lower base and
must still beat hunting. The bot travels to an adjacent tile, uses the map
object normally, verifies unique-ID storage and every cloned top-level root,
including aggregate count growth when a stackable root merges into existing
inventory and structural signatures for non-stackable roots. All top-level
roots must pass before the claim succeeds. The bot then opens the main backpack
and nested reward ancestors through bounded normal item-use actions. Open
container window `0` is reserved for corpses, `1` for the main backpack, and
`2` through `15` for at most 14 nested reward ancestors. A deeper path is
unsupported, and repeated failure to open one ancestor ends the objective.
Routine healing and eating wait through claim verification, nested equipment
access, and equipment-result verification so supplies cannot mutate the bundle
identity or interrupt displaced-item verification while either is still needed. A
selected nested upgrade is moved normally into its equipment slot;
the root container, sibling contents, and displaced equipment remain owned.
Bundles with positive known utility but no equipment upgrade complete after the
whole reward is verified. Transient objective and route state are not persisted;
after restart, persisted storage and equipment cause the next
candidate evaluation to skip completed or obsolete rewards. A claimed upgrade
still owned but not yet equipped resumes directly at equipment handling rather
than claiming again.

The top-level selector compares `service`, `pickup_reward`, and `hunt` only at
safe objective boundaries: normal startup, pickup completion/failure, completed
service/depot work, and a hunt deadline or capacity return. It does not run
during movement, combat, healing, looting, dialogue, transactions, or pending
item verification. Death recovery and focused gameplay modes preserve their
forced service/hunt contracts. Observed danger, missing healing supplies, and
other executor safety interrupts can force service without waiting for a normal
boundary. Critical healing can interrupt reward travel before a claim. After a
claim, routine upkeep waits for the bounded verification and equipment sequence.

Selection is intentionally deterministic and provisional. Hunt has utility
`300`; ordinary service starts at `400` and adds reserve, sale, and cash needs;
equipment pickup starts at `650`, economic pickup at `250`, and both add known
bundle utility before subtracting route steps.
Capacity service is at least `900`, and critical healing service is at least
`1000`. A successful pickup applies a five-minute in-memory family cooldown; a
failed or safety-interrupted pickup applies 60 seconds. This prevents immediate
reward chaining while retaining reproducible evidence for later calibration and
probabilistic preferences. Hunt feasibility is deferred when a higher-utility
goal already wins; when hunt can win, the normal region planner must produce a
suitable reachable region before selection. The selected region is carried into
the hunt executor rather than planned twice.

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
Persistent missing routes stop the controller after bounded retries. Three
repeated A-B reversals without a new best distance suppress the implicated
transition tiles for two minutes. A hunt drops that patrol point and continues;
other objectives replan around the suppression. The recovery emits
`navigation_progress` and, for hunts, `hunt_region_patrol`.

At startup and after the five-minute deadline or 30 oz capacity threshold, the
bot discovers live NPCs whose XML has an exact `playerbot_service` value of
`shop` or `banker`; other or missing values are ignored. For example:

```xml
<parameter key="playerbot_service" value="shop" />
```

Each tagged shop's `ShopModule` registers its loaded XML buy/sell offers on the
owning live NPC. A tagged shop without registered offers is ignored. The bot
uses that catalog to select the best sale price or nearest purchase provider
before moving, rather than visiting every shop to probe its window. The current
reachable Rookgaard allowlist contains shops Al Dee, Billy, Dixi, Elgar,
Hyacinth, Lee'Delle, Lily, Norma, Obi, Tom, and Willie, plus banker Paulie. This
allowlist is configuration, not inferred geographical knowledge. The selected
merchant is still greeted and its normal trade window is opened for each
transaction.

The bot sends `hi` through the selected NPC's normal speech handler and treats
any private reply from that same NPC as focus acknowledgement; reply text is
not interpreted. It then sends `trade` and requires that NPC to own the loaded
shop window before transacting. Greeting and shop-window setup each permit
three attempts; exhausted focus or window attempts emit
`npc_focus_unconfirmed` or `shop_window_unavailable` and stop the controller.
A service must also be reachable by the bounded map navigator and approached
within three tiles on the same floor; failed service navigation stops the
controller. Do not tag NPCs outside the bot's reachable region until service
selection accounts for route reachability and access requirements.

Outside the hunt phase, an adjacent monster actively targeting the bot
interrupts service, return, or deposit work. The bot attacks that immediate
threat without chase mode, does not loot it, and replans the unchanged
objective after the threat dies or disengages. It does not search for unrelated
monsters or pursue a threat that moves away. If several adjacent monsters are
attacking, the monster occupying the bot's pending navigation step is selected
first. The same priority remains during the temporary suppression window after
that step is confirmed blocked; distance and creature ID provide deterministic
fallback ordering.

The bot builds its item-value table from the loaded sell offers of tagged shop
NPCs. Currency uses its intrinsic worth; other items are eligible loot only
when a known shop buys them. Corpse contents are ranked by sell value per unit
weight. When capacity cannot hold a more profitable item, the bot may drop
lower-density, NPC-sellable backpack cargo if the complete replacement gains
value. Equipped items, containers, currency, and reserved tools, meat, and
potions are not replacement candidates. Unknown items remain in the corpse.
This is a Rookgaard-scale runtime catalog, not the eventual shared world-wide
economy index. Subtype-specific fluid and splash offers are excluded until the
inventory policy tracks subtype quantities.

At service time the same loaded offers determine what backpack surplus can be
sold; equipped items, ordinary containers, non-empty corpse containers, and
configured supply reserves are excluded. Other loot and nested bags are handled
by the fake-depot policy below. The bot buys exactly
any deficit to five small health potions (`8704`) and one meat (`2666`), then
uses the nearest matching live banker's dialogue to deposit all carried money
and withdraw 100 gp. NPC names and coordinates are telemetry, not controller
inputs.

At or below 60% health, the bot interrupts its current objective and uses one
carried small health potion on itself through normal use-with-creature handling.
It verifies both potion consumption and net health gain before attempting
another potion or resuming the unchanged objective. A missing potion is a
bounded supply outcome that redirects the bot to its existing service cycle;
unverified use and consumed potions without observed recovery are distinct
failures. An in-flight potion purchase is verified and its service stage is
advanced before the bot consumes the newly purchased stock. This is the first
health-maintenance slice, not general potion, spell, mana, condition, or
retreat planning.

After depositing remaining carried loot on the tile south of `(32105, 32195, 8)`,
normal operation selects a dynamic Rookgaard region and patrols its
spawn-adjacent destinations. Gameplay fixture modes retain the fixed
`(32084, 32144, 5)`, `(32103, 32124, 8)`, `(32117, 32090, 9)` sequence for
regression assertions. The bot returns after the configured hunt interval or
when free capacity falls below 30 oz and cancels attack/follow first.

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

When selecting a monster, the controller records its configured corpse item
from the loaded `MonsterType`. A corpse is binary-lootable only when that loaded
item is both a server-identified corpse and a container. After death, any other
configuration emits `corpse_not_lootable` as a skipped result and resumes the
interrupted objective without searching for or opening the death item. This
classification does not cover skinning, special-use corpses, or other secondary
corpse actions.

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
Startup reconstructs pickup-reward candidates from persisted player and loaded
world state, then begins a fresh service cycle when none is selected.
Death preserves the normal corpse, penalties, save, removal, and temple login
position. The manager retains server ownership while the bot is offline, waits
for the configured relog delay, loads the same persistent character, and creates
a fresh controller in the service phase. No pre-death target, route, container,
NPC conversation, pending action, or objective state is resumed.

Deaths within five minutes of the previous login use exponential relog backoff,
capped at 60 seconds. A lifetime of at least five minutes resets that count.
The configured death limit permits that many consecutive recoveries and
abandons the next death. A failed character load permits three total relog
attempts: the initial attempt and two retries. Server ownership and human-login
blocking remain in force during all recovery delays, failures, retries, and
abandonment outcomes until playerbots are disabled and the server is recreated.
Removing the Compose volume resets the bot and all other local world state.

`PLAYERBOT_HUNT_DURATION_SECONDS` changes the hunt window at server startup and
defaults to `300`. Invalid text falls back to `300`; zero and negative values
are clamped to one second. Recreate the server container after changing it.

`PLAYERBOT_RELOG_DELAY_SECONDS` sets the initial post-death delay and defaults
to `5`; repeated deaths multiply it by two up to 60 seconds.
`PLAYERBOT_MAX_CONSECUTIVE_DEATHS` defaults to `3` and bounds automatic recovery
from a repeated death loop. Invalid text falls back to the respective default;
zero and negative values are clamped to one. Recreate the server container after
changing either value.

Normal Rookgaard hunts derive same-floor regions by joining overlapping
eight-tile spawn heat kernels within a bounded 48-by-48 contour. Loaded monster
health, attacks, armor, experience, spawn chance, and interval are scored against
the bot's current health, equipment defense, weapon, and skill. Floor transitions
remain region boundaries. The existing navigator validates the highest-scoring
candidates; selected regions supply spawn-adjacent patrol destinations. The
prototype accepts spawn positions on floors 6 through 15 inside the rectangle
extending 180 tiles in each axis from temple `(32097, 32219, 7)`.
This is an axis-aligned square cutoff: X and Y are each checked independently.
It is not a radial distance or a verified boundary for all of Rookgaard.
Each configured hunt/service cycle triggers rescoring, while excessive observed
damage temporarily excludes a region. Level gains are recorded in hunt outcomes
but do not interrupt the fixed hunt interval. Gameplay fixture modes retain their
fixed regression destinations.

Candidate scoring projects net experience for the configured hunt interval.
Spawn XP supply is multiplied by the active server experience stage, and route
movement/use time is deducted from available hunting time. Completed hunts keep
a bounded rolling correction from observed versus projected XP per minute for
that region; unobserved regions start at `1.0`. A sample requires at least 30
seconds and one kill, is clamped to `0.25` through `2.0`, and uses a 65/35 rolling
blend after the first sample. Observations are controller memory and reset on
relog or server restart. Threat remains a separate safety gate rather than a
reward penalty in the XP score.

Predicted threat rejects a region above `0.35` expected fight damage per maximum
health. It models up to three locally overlapping hostile spawns, reducing
combined DPS as each target is expected to die. Actual damage totaling one
maximum-health pool within the first two minutes abandons the region and applies
a ten-minute controller cooldown.

When the exact character `GOD Admin` is online, objective changes, hunt
selections/outcomes, and successful verified deposit, sale, purchase, and bank
transactions are sent directly as both `Bot One` private messages and orange
console status text. These transient diagnostics bypass creature-speech hooks;
JSONL remains the durable authoritative log.

`PLAYERBOT_SPEED_BONUS` adds a movement-speed delta from `0` through `1000` to
server-owned bots after login and defaults to `300`; out-of-range values are
clamped and invalid text falls back to `300`. It accelerates local development
and observation without changing combat or action delays.
Recreate the server after changing it.

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

The prototype advances progression issue #57, the mainland loop tracked in
issue #22, and navigation architecture tracked in issue #28 without closing
those umbrellas. See
[`testing.md`](testing.md) for validation commands.
